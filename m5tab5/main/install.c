/*
 * install.c -- see install.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "install.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

#include "vol.h"

static const char *TAG = "install";

#define NVS_NS      "duplicitous"
#define NVS_KEY     "app2"
#define CHUNK       (32 * 1024)

/* A merged image's partition table, and the shape of one entry. */
#define PT_OFFSET   0x8000
#define PT_MAGIC    0x50AA
#define PT_MAX      95
typedef struct __attribute__((packed)) {
    uint16_t magic;
    uint8_t  type;
    uint8_t  subtype;
    uint32_t offset;
    uint32_t size;
    char     label[16];
    uint32_t flags;
} pt_entry_t;

static SemaphoreHandle_t s_work;

void work_lock(void)
{
    static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    taskENTER_CRITICAL(&mux);
    if (!s_work) s_work = xSemaphoreCreateRecursiveMutex();
    taskEXIT_CRITICAL(&mux);
    xSemaphoreTakeRecursive(s_work, portMAX_DELAY);
}

void work_unlock(void) { xSemaphoreGiveRecursive(s_work); }

static const esp_partition_t *app2(void)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                    ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
}

#define FAIL(...) do { snprintf(err, err_len, __VA_ARGS__); ret = ESP_FAIL; goto out; } while (0)

esp_err_t install_from_file(const char *path, install_progress_fn cb, void *ctx,
                            char *err, size_t err_len)
{
    esp_err_t ret = ESP_OK;
    FILE *f = NULL;
    uint8_t *buf = NULL;
    esp_ota_handle_t ota = 0;
    bool ota_open = false;
    storage_id_t vid = STORAGE_COUNT;
    err[0] = '\0';

    work_lock();
    const esp_partition_t *dest = app2();
    if (!dest) FAIL("no app2 partition in this table");
    if (!vol_hold(path, &vid)) FAIL("%s is not on a mounted card or drive", path);

    struct stat st;
    if (stat(path, &st) != 0) FAIL("cannot read %s", path);
    f = fopen(path, "rb");
    if (!f) FAIL("cannot open %s", path);
    const size_t fsize = (size_t)st.st_size;

    /* Where the app starts in the file, and how long it can be. */
    size_t start = 0, len = fsize;
    uint8_t first;
    if (fread(&first, 1, 1, f) != 1) FAIL("empty file");
    if (first != ESP_IMAGE_HEADER_MAGIC) {
        if (fsize <= PT_OFFSET + sizeof(pt_entry_t)) FAIL("not an ESP image");
        if (fseek(f, PT_OFFSET, SEEK_SET) != 0) FAIL("seek failed");
        bool found = false;
        for (int i = 0; i < PT_MAX; i++) {
            pt_entry_t e;
            if (fread(&e, sizeof e, 1, f) != 1 || e.magic != PT_MAGIC) break;
            if (e.type == 0 /* app */) {
                start = e.offset;
                len = e.size;
                found = true;
                break;
            }
        }
        if (!found) FAIL("not an app, and no partition table at 0x8000");
        if (start >= fsize) FAIL("merged image ends before its app (0x%x)", (unsigned)start);
        if (len > fsize - start) len = fsize - start;
    }

    esp_image_header_t hdr;
    if (fseek(f, start, SEEK_SET) != 0 || fread(&hdr, sizeof hdr, 1, f) != 1)
        FAIL("short file");
    if (hdr.magic != ESP_IMAGE_HEADER_MAGIC) FAIL("no app image at 0x%x", (unsigned)start);
    if (hdr.chip_id != ESP_CHIP_ID_ESP32P4)
        FAIL("built for chip id %u, not the ESP32-P4 (%u)", hdr.chip_id, ESP_CHIP_ID_ESP32P4);
    if (len > dest->size)
        FAIL("%u KB does not fit app2 (%u KB)", (unsigned)(len / 1024), (unsigned)(dest->size / 1024));

    buf = heap_caps_malloc(CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = malloc(CHUNK);
    if (!buf) FAIL("out of memory");

    ESP_LOGI(TAG, "%s: app at 0x%x, %u bytes -> %s", path, (unsigned)start, (unsigned)len, dest->label);
    /*
     * Erase as we go, a sector ahead of each write, not len up front.
     * esp_ota_begin(dest, len) erases the whole image's length in one call
     * -- seconds, with nothing redrawn -- and while flash is erasing the
     * cache is held and the panel's DMA can miss its PSRAM fetches: an
     * underrun, which on this panel is a cyan strobe (seen between
     * "Installing" and the bar; esp_lcd logs "underrun happens" for each).
     * Sector by sector, each stall is short and the bar moves throughout.
     * What lies past the image in app2 is left as it was: the bootloader
     * and esp_ota_end() stop at the image's own length.
     */
    esp_err_t e = esp_ota_begin(dest, OTA_WITH_SEQUENTIAL_WRITES, &ota);
    if (e != ESP_OK) FAIL("ota begin: %s", esp_err_to_name(e));
    ota_open = true;

    fseek(f, start, SEEK_SET);
    size_t done = 0;
    if (cb) cb(0, len, ctx);
    while (done < len) {
        size_t want = len - done < CHUNK ? len - done : CHUNK;
        size_t got = fread(buf, 1, want, f);
        if (got != want) FAIL("read failed at %u", (unsigned)(start + done));
        e = esp_ota_write(ota, buf, got);
        if (e != ESP_OK) FAIL("flash write: %s", esp_err_to_name(e));
        done += got;
        if (cb) cb(done, len, ctx);
    }
    ota_open = false;
    e = esp_ota_end(ota);   /* verifies the image: checksum, hash, chip */
    if (e != ESP_OK) FAIL("image did not verify: %s", esp_err_to_name(e));

    /* Remember where it came from, for the menu. */
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        const char *base = strrchr(path, '/');
        nvs_set_str(h, NVS_KEY, base ? base + 1 : path);
        nvs_commit(h);
        nvs_close(h);
    }

out:
    if (ota_open) esp_ota_abort(ota);
    free(buf);
    if (f) fclose(f);
    vol_release(vid);
    work_unlock();
    if (ret != ESP_OK) ESP_LOGW(TAG, "%s: %s", path, err);
    return ret;
}

bool install_present(char *name, size_t name_len)
{
    const esp_partition_t *p = app2();
    esp_app_desc_t desc;
    if (!p || esp_ota_get_partition_description(p, &desc) != ESP_OK) return false;
    if (name && name_len) {
        nvs_handle_t h;
        size_t n = name_len;
        bool got = false;
        if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
            got = nvs_get_str(h, NVS_KEY, name, &n) == ESP_OK;
            nvs_close(h);
        }
        if (!got) snprintf(name, name_len, "%s", desc.project_name);
    }
    return true;
}

bool install_came_back_by_reset(void)
{
    const esp_partition_t *p = app2();
    esp_ota_img_states_t st;
    return p && esp_ota_get_state_partition(p, &st) == ESP_OK && st == ESP_OTA_IMG_ABORTED;
}

esp_err_t install_boot(void)
{
    const esp_partition_t *p = app2();
    if (!p) return ESP_ERR_NOT_FOUND;
    /* Marks it NEW: the bootloader runs it once, and falls back to us
     * at the reset after, unless the app confirms itself. */
    esp_err_t e = esp_ota_set_boot_partition(p);
    if (e != ESP_OK) return e;
    esp_restart();
}
