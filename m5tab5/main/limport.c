/*
 * limport.c -- see limport.h. The files and the volumes; limport_core.c
 * has everything that does not need ESP-IDF.
 *
 * SPDX-License-Identifier: MIT
 */
#include "limport.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "install.h"        /* work_lock() */
#include "limport_core.h"
#include "storage.h"
#include "vol.h"
#include "wifistore.h"

static const char *TAG = "limport";

#define CONFIG_MAX      (256 * 1024)
#define NETS_MAX        32
#define BIN_MIN         (64 * 1024)
#define BIN_MAX         (8 * 1024 * 1024)
#define BINS_MAX        64
#define DEPTH_MAX       2
#define CHUNK           4096
#define PATH_MAX_LI     200

#define PS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

static bool is_bin(const char *n)
{
    const size_t l = strlen(n);
    return l > 4 && !strcasecmp(n + l - 4, ".bin");
}

/* .bin files under dir, DEPTH_MAX folders down; paths into `out`. */
static void collect_bins(const char *dir, int depth, char (*out)[PATH_MAX_LI], int *n)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && *n < BINS_MAX) {
        if (e->d_name[0] == '.') continue;
        char path[PATH_MAX_LI];
        if (snprintf(path, sizeof path, "%s/%s", dir, e->d_name) >= (int)sizeof path) continue;
        if (e->d_type == DT_DIR) {
            if (depth < DEPTH_MAX) collect_bins(path, depth + 1, out, n);
        } else if (is_bin(e->d_name)) {
            struct stat st;
            if (stat(path, &st) == 0 && st.st_size >= BIN_MIN && st.st_size <= BIN_MAX) {
                memcpy(out[(*n)++], path, strlen(path) + 1);
            }
        }
    }
    closedir(d);
}

/* One read pass over a file; the callback sees each chunk. False on
 * a read error or when it says stop. */
static bool pass(const char *path, uint8_t *buf, bool (*fn)(void *, const uint8_t *, size_t), void *ctx)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    size_t n;
    bool stop = false;
    while (!stop && (n = fread(buf, 1, CHUNK, f)) > 0) {
        stop = fn(ctx, buf, n);
        vTaskDelay(1);      /* a full image is seconds of CPU: let IDLE0 run, feed the watchdog */
    }
    fclose(f);
    return stop;
}

typedef struct { li_marker_t m; } marker_ctx_t;
static bool marker_cb(void *c, const uint8_t *b, size_t n) { return li_marker_feed(&((marker_ctx_t *)c)->m, b, n); }

typedef struct { li_scan_t s; const li_net_t *nets; int nn; uint8_t key[16]; } scan_ctx_t;
static bool scan_cb(void *c, const uint8_t *b, size_t n)
{
    scan_ctx_t *x = c;
    return li_scan_feed(&x->s, b, n, x->nets, x->nn, x->key);
}

static bool find_key(const char *path, uint8_t *buf, const li_net_t *nets, int nn, uint8_t key[16])
{
    marker_ctx_t mc;
    li_marker_init(&mc.m);
    if (!pass(path, buf, marker_cb, &mc)) return false;       /* not M5Launcher's */

    scan_ctx_t *sc = heap_caps_malloc(sizeof *sc, PS);
    if (!sc) return false;
    li_scan_init(&sc->s);
    sc->nets = nets;
    sc->nn = nn;
    bool found = pass(path, buf, scan_cb, sc);
    if (!found) found = li_scan_end(&sc->s, nets, nn, sc->key);
    if (found) memcpy(key, sc->key, 16);
    memset(sc, 0, sizeof *sc);
    heap_caps_free(sc);
    return found;
}

static char *load_config(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > CONFIG_MAX) { fclose(f); return NULL; }
    char *t = heap_caps_malloc((size_t)sz + 1, PS);
    if (!t) { fclose(f); return NULL; }
    const size_t rd = fread(t, 1, (size_t)sz, f);
    fclose(f);
    t[rd] = '\0';
    return t;
}

static limport_result_t from_volume(storage_id_t id, int *imported, int *skipped)
{
    char cfg[PATH_MAX_LI];
    snprintf(cfg, sizeof cfg, "%s/config.conf", storage_mount_path(id));
    char *json = load_config(cfg);
    if (!json) return LIMPORT_NO_CONFIG;

    li_net_t *nets = heap_caps_malloc(NETS_MAX * sizeof *nets, PS);
    if (!nets) { heap_caps_free(json); return LIMPORT_NO_MEM; }
    int skip = 0;
    const int nn = li_parse_config(json, strlen(json), nets, NETS_MAX, &skip);
    heap_caps_free(json);
    if (nn < 0) { heap_caps_free(nets); return LIMPORT_NO_CONFIG; }
    if (nn == 0) { *skipped += skip; heap_caps_free(nets); return LIMPORT_NO_NETWORKS; }

    /* The key: a .bin on this volume first, then the other one. */
    char (*bins)[PATH_MAX_LI] = heap_caps_malloc(BINS_MAX * PATH_MAX_LI, PS);
    uint8_t *buf = heap_caps_malloc(CHUNK, PS);
    if (!bins || !buf) {
        heap_caps_free(bins); heap_caps_free(buf); heap_caps_free(nets);
        return LIMPORT_NO_MEM;
    }
    uint8_t key[16];
    bool found = false;
    for (int pass_no = 0; pass_no < 2 && !found; pass_no++) {
        for (int v = 0; v < STORAGE_COUNT && !found; v++) {
            const storage_id_t vid = (storage_id_t)v;
            if ((vid == id) != (pass_no == 0) || !storage_present(vid)) continue;
            char root[16];
            snprintf(root, sizeof root, "%s", storage_mount_path(vid));
            storage_id_t held;
            if (vol_hold(root, &held)) {
                int nb = 0;
                collect_bins(root, 0, bins, &nb);
                for (int i = 0; i < nb && !found; i++) {
                    found = find_key(bins[i], buf, nets, nn, key);
                    if (found) ESP_LOGI(TAG, "key found in %s", bins[i]);
                }
            }
            vol_release(held);
        }
    }
    heap_caps_free(bins);
    heap_caps_free(buf);

    limport_result_t res = LIMPORT_NO_KEY;
    if (found) {
        /* Never log a passphrase, including on failure. */
        char pw[LI_CT_MAX + 1];
        for (int i = 0; i < nn; i++) {
            const int l = li_decrypt_net(key, &nets[i], pw, sizeof pw);
            bool is_psk = false;
            if (l < 0 || !li_classify(pw, &is_psk)) { ESP_LOGW(TAG, "'%s': not usable", nets[i].ssid); (*skipped)++; }
            else if (wifistore_save(nets[i].ssid, pw, is_psk) == ESP_OK) (*imported)++;
            else (*skipped)++;
            memset(pw, 0, sizeof pw);
        }
        *skipped += skip;
        res = LIMPORT_DONE;
    }
    memset(key, 0, sizeof key);
    memset(nets, 0, NETS_MAX * sizeof *nets);
    heap_caps_free(nets);
    return res;
}

limport_result_t limport_run(int *imported, int *skipped)
{
    *imported = *skipped = 0;
    limport_result_t best = LIMPORT_NO_CONFIG;
    work_lock();
    for (int v = 0; v < STORAGE_COUNT; v++) {
        const storage_id_t id = (storage_id_t)v;
        if (!storage_present(id)) continue;
        char root[16];
        snprintf(root, sizeof root, "%s", storage_mount_path(id));
        storage_id_t held;
        const bool mounted = vol_hold(root, &held);
        limport_result_t r = LIMPORT_NO_CONFIG;
        if (mounted) r = from_volume(id, imported, skipped);
        vol_release(held);
        if (r == LIMPORT_DONE) { best = r; break; }
        if (r > best || best == LIMPORT_NO_CONFIG) best = r;   /* the furthest it got */
    }
    work_unlock();
    ESP_LOGW(TAG, "import: %d saved, %d skipped (result %d)", *imported, *skipped, (int)best);
    return best;
}
