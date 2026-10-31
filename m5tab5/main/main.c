/*
 * Avoidant for the M5Stack Tab5 -- choose a .bin on the microSD card,
 * or a USB drive, write it to app2 and boot it, in the manner of
 * bmorcelli/Launcher.
 *
 * At power-on, if app2 holds an app, it boots after a short countdown
 * unless the screen is touched. Otherwise, or after a touch, the card and
 * the drive are listed, then their folders and .bin files; tapping a .bin asks, writes and
 * boots it. An .elf -- an app built against feckless-abi-for-m5tab5 --
 * is run instead, on this board as it is, without a restart (0023). The console on the USB-C port (console.h) does the same
 * from a computer, and sends files to either.
 *
 * SPDX-License-Identifier: MIT
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lcd_panel_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_handler.h"     /* 0035 */

#include "storage.h"
#include "storage_io.h"
#include "usbhost.h"
#include "feckless_net.h"     /* 0043 */

#include "gfx.h"
#include "lcd.h"
#include "tab5io.h"
#include "touch.h"

#include "console.h"
#include "elfrun.h"
#include "feckless_abi.h"
#include "install.h"
#include "libversions.h"     /* 0028: generated, cmake/libversions.cmake */
#include "limport.h"         /* 0045 */
#include "settings.h"        /* 0028 */
#include "vol.h"
#include "xfer.h"

static const char *TAG = "avoidant";

#define AUTOBOOT_MS     3000

/*
 * Phase markers, for finding a cyan screen (a DSI underrun). IDF's
 * esp_lcd logs each underrun from its ISR ("can't fetch data from
 * external memory fast enough, underrun happens"); these say what was
 * running at the time, in the same log, with a millisecond clock.
 * Warnings, so the console's WARN level lets them through. Temporary:
 * out again once the cause is found (ARCHITECTURE.md, 0013).
 */
#define PHASE(...) ESP_LOGW("phase", __VA_ARGS__)
#define BRIGHTNESS      60

/* Layout, portrait 720 x 1280. */
#define W           LCD_H_RES
#define H           LCD_V_RES
#define PAD         24
#define SCALE       3
#define TEXT_H      GFX_GLYPH_H(SCALE)
#define HEAD_H      140
#define ROW_H       88
#define FOOT_H      130
#define LIST_Y      HEAD_H
#define ROWS        ((H - HEAD_H - FOOT_H) / ROW_H)

#define C_BG        RGB(16, 18, 24)
#define C_HEAD      RGB(32, 36, 48)
#define C_TEXT      RGB(235, 235, 235)
#define C_DIM       RGB(140, 145, 160)
#define C_DIR       RGB(120, 190, 255)
#define C_BIN       RGB(255, 210, 120)
#define C_ELF       RGB(170, 225, 140)     /* 0023: runs, does not install */
#define C_RULE      RGB(44, 48, 60)
#define C_BTN       RGB(52, 58, 76)
#define C_GO        RGB(40, 130, 70)
#define C_BAD       RGB(220, 90, 80)

/* ---- touch: one tap at a time ------------------------------------------ */

/* Wait up to ms (forever if < 0) for a finger to touch and lift; the tap
 * is where it touched. False on timeout. */
static bool wait_tap(int *x, int *y, int ms)
{
    int64_t until = ms < 0 ? INT64_MAX : esp_timer_get_time() + (int64_t)ms * 1000;
    bool down = false;
    for (;;) {
        int tx, ty;
        bool now = touch_get(&tx, &ty);
        if (now && !down) { down = true; *x = tx; *y = ty; }
        if (!now && down) return true;
        if (!down && esp_timer_get_time() > until) return false;
        vTaskDelay(pdMS_TO_TICKS(15));
    }
}

static bool hit(int x, int y, int bx, int by, int bw, int bh)
{
    return x >= bx && x < bx + bw && y >= by && y < by + bh;
}

/* ---- drawing ------------------------------------------------------------ */

static void text(int x, int y, const char *s, int max_w, uint16_t c)
{
    gfx_draw_text(x, y, s, SCALE, max_w, c);
}

static void text_centred(int y, const char *s, uint16_t c)
{
    int w = gfx_text_w(s, SCALE);
    text(w < W - 2 * PAD ? (W - w) / 2 : PAD, y, s, W - 2 * PAD, c);
}

static void button(int x, int y, int w, int h, const char *label, uint16_t bg)
{
    gfx_fill_rect(x, y, w, h, bg);
    int tw = gfx_text_w(label, SCALE);
    text(x + (w - tw) / 2, y + (h - TEXT_H) / 2, label, w, C_TEXT);
}

static void header(const char *title, const char *sub)
{
    gfx_fill_rect(0, 0, W, HEAD_H, C_HEAD);
    text(PAD, 28, title, W - 2 * PAD, C_TEXT);
    if (sub) gfx_draw_text_tail(PAD, 28 + TEXT_H + 18, sub, SCALE, W - 2 * PAD, C_DIM);
}

static void show(void) { gfx_blit(0, H); }

/* A message and one OK button. */
static void message(const char *title, const char *body, uint16_t c)
{
    gfx_fill_rect(0, 0, W, H, C_BG);
    header(title, NULL);
    gfx_draw_para(PAD, HEAD_H + 40, body, SCALE, W - 2 * PAD, TEXT_H + 10, 20, c);
    button(PAD, H - FOOT_H + 20, W - 2 * PAD, FOOT_H - 40, "OK", C_BTN);
    show();
    int x, y;
    do wait_tap(&x, &y, -1); while (!hit(x, y, 0, H - FOOT_H, W, FOOT_H));
}

/* ---- the gear and its page (0028) -------------------------------------- */

/* The gear, top right of the menu's header: a hub, eight teeth round it
 * and a hole, drawn rather than a glyph -- the fonts have none. */
#define GEAR_X      (W - PAD - 34)
#define GEAR_Y      (HEAD_H / 2)

static void draw_gear(void)
{
    for (int i = 0; i < 8; i++) {
        static const int dx[8] = { 0, 21, 30, 21, 0, -21, -30, -21 };
        static const int dy[8] = { -30, -21, 0, 21, 30, 21, 0, -21 };
        gfx_fill_circle(GEAR_X + dx[i], GEAR_Y + dy[i], 8, C_DIM);
    }
    gfx_fill_circle(GEAR_X, GEAR_Y, 25, C_DIM);
    gfx_fill_circle(GEAR_X, GEAR_Y, 10, C_HEAD);
}

/* Generous: the whole top-right square of the header. */
static bool gear_hit(int x, int y)
{
    return y < HEAD_H && x >= W - HEAD_H;
}

/* 0045: Wi-Fi networks from M5Launcher's config.conf. Seconds long, on this
 * task: the screen says so first, and the result is a message. */
static void import_wifi(void)
{
    gfx_fill_rect(0, 0, W, H, C_BG);
    header("Wi-Fi networks", "from M5Launcher");
    text_centred(H / 2 - TEXT_H, "Looking for the key...", C_TEXT);
    text_centred(H / 2 + 20, "This takes a while.", C_DIM);
    show();

    int imported, skipped;
    const limport_result_t r = limport_run(&imported, &skipped);
    char body[200];
    uint16_t c = C_BAD;
    switch (r) {
    case LIMPORT_DONE:
        snprintf(body, sizeof body, "Saved %d network%s%s. Apps that use the network list have them now.",
                 imported, imported == 1 ? "" : "s",
                 skipped ? " (the rest were open, damaged or already unusable)" : "");
        c = C_TEXT;
        break;
    case LIMPORT_NO_CONFIG:
        snprintf(body, sizeof body, "No config.conf with a Wi-Fi list on the card or the drive.");
        break;
    case LIMPORT_NO_NETWORKS:
        snprintf(body, sizeof body, "config.conf has no secured networks to import.");
        break;
    case LIMPORT_NO_KEY:
        snprintf(body, sizeof body,
                 "Found the networks but not their key. Put the M5Launcher .bin it came from "
                 "on the card or the drive, in the top folder or one folder down.");
        break;
    default:
        snprintf(body, sizeof body, "Out of memory.");
        break;
    }
    message("Wi-Fi networks", body, c);
}

static const struct { esp_log_level_t level; const char *name; } k_levels[] = {
    { ESP_LOG_NONE, "None" }, { ESP_LOG_ERROR, "Error" }, { ESP_LOG_WARN, "Warn" },
    { ESP_LOG_INFO, "Info" }, { ESP_LOG_DEBUG, "Debug" },
};
#define N_LEVELS    (int)(sizeof k_levels / sizeof k_levels[0])

/*
 * The console's log level, five buttons, the one in force lit; and under
 * it the libraries this build was made from, with their versions and
 * licences, from libversions.h. A tap on a level applies it at once and
 * keeps it. Back returns to the menu.
 */
static void settings_page(void)
{
    const int lscale = SCALE - 1, lh = GFX_GLYPH_H(lscale) + 10;
    const int bw = (W - (N_LEVELS + 1) * PAD / 2) / N_LEVELS, bh = 80;
    const int by = HEAD_H + 30 + TEXT_H + 20;
    const int fy = H - FOOT_H + 20, fh = FOOT_H - 40;
    const int ih = 80;
    for (;;) {
        const esp_log_level_t cur = settings_log_level();
        gfx_fill_rect(0, 0, W, H, C_BG);
        header("Settings", avoidant_title());

        text(PAD, HEAD_H + 30, "Console log level", W - 2 * PAD, C_TEXT);
        for (int i = 0; i < N_LEVELS; i++)
            button(PAD / 2 + i * (bw + PAD / 2), by, bw, bh, k_levels[i].name,
                   k_levels[i].level == cur ? C_GO : C_BTN);
        int y = by + bh + 16;
        gfx_draw_text(PAD, y, "On the USB-C port, Avoidant's and a running app's.",
                      lscale, W - 2 * PAD, C_DIM);
        y += lh;
        gfx_draw_text(PAD, y, "Off during a transfer, whatever this says.",
                      lscale, W - 2 * PAD, C_DIM);
        y += lh + 30;

        gfx_fill_rect(0, y - 16, W, 2, C_RULE);             /* 0045 */
        text(PAD, y, "Wi-Fi networks", W - 2 * PAD, C_TEXT);
        y += TEXT_H + 16;
        const int iy = y;
        button(PAD, iy, W - 2 * PAD, ih, "Import from M5Launcher", C_BTN);
        y += ih + 16;
        gfx_draw_text(PAD, y, "Needs config.conf and the .bin it came from.",
                      lscale, W - 2 * PAD, C_DIM);
        y += lh + 30;

        gfx_fill_rect(0, y - 16, W, 2, C_RULE);
        text(PAD, y, "Libraries", W - 2 * PAD, C_TEXT);
        y += TEXT_H + 16;
        /* three columns: module, its checkout, its licence */
        const int cx1 = PAD, cx2 = W * 52 / 100, cx3 = W * 74 / 100;
        int shown = 0;
        for (int i = 0; i < LIBVERSIONS_COUNT && y + lh <= fy - 10; i++, shown++, y += lh) {
            gfx_draw_text(cx1, y, k_libversions[i][0], lscale, cx2 - cx1 - 12, C_TEXT);
            gfx_draw_text(cx2, y, k_libversions[i][1], lscale, cx3 - cx2 - 12, C_DIM);
            gfx_draw_text(cx3, y, k_libversions[i][2], lscale, W - PAD - cx3, C_DIM);
        }
        if (shown < LIBVERSIONS_COUNT) {
            char more[32];
            snprintf(more, sizeof more, "and %d more", LIBVERSIONS_COUNT - shown);
            gfx_draw_text(cx1, y, more, lscale, W - 2 * PAD, C_DIM);
        }

        button(PAD, fy, W - 2 * PAD, fh, "Back", C_BTN);
        show();

        int x, ty;
        wait_tap(&x, &ty, -1);
        if (hit(x, ty, 0, H - FOOT_H, W, FOOT_H)) return;
        if (hit(x, ty, PAD, iy, W - 2 * PAD, ih)) { import_wifi(); continue; }     /* 0045 */
        for (int i = 0; i < N_LEVELS; i++)
            if (hit(x, ty, PAD / 2 + i * (bw + PAD / 2), by, bw, bh)) {
                settings_set_log_level(k_levels[i].level);
                ESP_LOGW(TAG, "console log level: %s", k_levels[i].name);
            }
    }
}

/* ---- the card listing --------------------------------------------------- */

typedef struct {
    char *name;
    bool dir;
    long size;
} entry_t;

static entry_t *s_ents;
static int s_n;
static int s_hidden;     /* .bin files here that are not apps */
/* "" is the list of volumes; otherwise a directory on one. */
static char s_dir[512] = "";

static bool is_bin(const char *n)
{
    size_t l = strlen(n);
    return l > 4 && !strcasecmp(n + l - 4, ".bin");
}

static bool is_elf(const char *n)
{
    size_t l = strlen(n);
    return l > 4 && !strcasecmp(n + l - 4, ".elf");
}

static int ent_cmp(const void *a, const void *b)
{
    const entry_t *x = a, *y = b;
    if (x->dir != y->dir) return x->dir ? -1 : 1;
    return strcasecmp(x->name, y->name);
}

static void free_list(void)
{
    for (int i = 0; i < s_n; i++) free(s_ents[i].name);
    free(s_ents);
    s_ents = NULL;
    s_n = 0;
}

/* Folders and .bin files in s_dir, folders first. Hidden ones (a leading
 * dot, macOS's ._ files among them) are left out. */
static bool add(const char *name, bool dir, long size)
{
    static int cap;
    if (s_n == 0) cap = 0;
    if (s_n == cap) {
        int c = cap ? cap * 2 : 32;
        entry_t *n = realloc(s_ents, c * sizeof *n);
        if (!n) return false;
        s_ents = n;
        cap = c;
    }
    s_ents[s_n].name = strdup(name);
    if (!s_ents[s_n].name) return false;
    s_ents[s_n].dir = dir;
    s_ents[s_n].size = size;
    s_n++;
    return true;
}

static void scan(void)
{
    free_list();
    s_hidden = 0;
    if (!s_dir[0]) {
        /* the volumes, in storage_id_t order: the card, then the drive */
        for (int i = 0; i < STORAGE_COUNT; i++)
            if (storage_present((storage_id_t)i)) add(storage_mount_path((storage_id_t)i) + 1, true, -1);
        return;
    }
    work_lock();
    storage_id_t vid;
    if (!vol_hold(s_dir, &vid)) {
        /* it went away while we were in it */
        vol_release(vid);
        work_unlock();
        s_dir[0] = '\0';
        scan();
        return;
    }
    DIR *d = opendir(s_dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            bool dir = e->d_type == DT_DIR;
            if (e->d_name[0] == '.' || (!dir && !is_bin(e->d_name) && !is_elf(e->d_name))) continue;
            if (!strcasecmp(e->d_name, "System Volume Information")) continue;
            long size = -1;
            if (!dir) {
                char full[800];
                struct stat st;
                snprintf(full, sizeof full, "%s/%s", s_dir, e->d_name);
                if (stat(full, &st) == 0) size = (long)st.st_size;
                /* only what install would take: a project's settings
                 * saved as .bin, or another chip's firmware, is left out
                 * here rather than refused after "Install?" */
                char why[96];
                if (is_elf(e->d_name)) {
                    /* 0023: an .elf that is not a Tab5 app is only logged;
                     * s_hidden counts .bins */
                    if (!elfrun_probe(full, why, sizeof why)) {
                        PHASE("not listed: %s (%s)", e->d_name, why);
                        continue;
                    }
                } else if (!install_probe(full, why, sizeof why)) {
                    PHASE("not listed: %s (%s)", e->d_name, why);
                    s_hidden++;
                    continue;
                }
            }
            if (!add(e->d_name, dir, size)) break;
        }
        closedir(d);
    }
    vol_release(vid);
    work_unlock();
    if (s_n) qsort(s_ents, s_n, sizeof *s_ents, ent_cmp);
}

static void fmt_size(char *out, size_t n, long size)
{
    if (size < 0) snprintf(out, n, "?");
    else if (size < 1024 * 1024) snprintf(out, n, "%ld KB", (size + 1023) / 1024);
    else snprintf(out, n, "%.1f MB", size / (1024.0 * 1024.0));
}

/* ---- install and boot ---------------------------------------------------- */

static void draw_progress(size_t done, size_t total, void *ctx)
{
    const char *name = ctx;
    static int last_pct = -1;
    int pct = total ? (int)((done * 100) / total) : 0;
    if (done != 0 && pct == last_pct) return;
    last_pct = pct;
    const int y = H / 2;
    gfx_fill_rect(0, HEAD_H, W, H - HEAD_H, C_BG);
    text_centred(y - 120, name, C_TEXT);
    gfx_fill_rect(PAD, y, W - 2 * PAD, 40, C_BTN);
    gfx_fill_rect(PAD, y, (int)((W - 2 * PAD) * (int64_t)done / (total ? total : 1)), 40, C_GO);
    char s[48];
    snprintf(s, sizeof s, "%d%%  (%u / %u KB)", pct, (unsigned)(done / 1024), (unsigned)(total / 1024));
    text_centred(y + 70, s, C_DIM);
    gfx_blit(HEAD_H, H);
}

static void install_and_boot(const entry_t *e)
{
    char path[800], size[24];
    snprintf(path, sizeof path, "%s/%s", s_dir, e->name);
    fmt_size(size, sizeof size, e->size);

    /* ask */
    gfx_fill_rect(0, 0, W, H, C_BG);
    header("Install to app2?", path);
    text_centred(H / 2 - 100, e->name, C_BIN);
    text_centred(H / 2 - 30, size, C_DIM);
    text_centred(H / 2 + 40, "It replaces what app2 holds.", C_DIM);
    const int bw = (W - 3 * PAD) / 2, by = H - FOOT_H + 20, bh = FOOT_H - 40;
    button(PAD, by, bw, bh, "Cancel", C_BTN);
    button(2 * PAD + bw, by, bw, bh, "Install", C_GO);
    show();
    int x, y;
    for (;;) {
        wait_tap(&x, &y, -1);
        if (hit(x, y, PAD, by, bw, bh)) return;
        if (hit(x, y, 2 * PAD + bw, by, bw, bh)) break;
    }

    /* write */
    gfx_fill_rect(0, 0, W, H, C_BG);
    header("Installing", path);
    show();
    char err[128];
    if (install_from_file(path, draw_progress, (void *)e->name, err, sizeof err) != ESP_OK) {
        message("Not installed", err, C_BAD);
        return;
    }

    /* boot */
    gfx_fill_rect(0, HEAD_H, W, H - HEAD_H, C_BG);
    text_centred(H / 2, "Booting...", C_TEXT);
    gfx_blit(HEAD_H, H);
    esp_err_t be = install_boot();
    snprintf(err, sizeof err, "Installed, but could not boot it: %s", esp_err_to_name(be));
    message("Not booted", err, C_BAD);
}

/* ---- run an .elf (0023) -------------------------------------------------- */

/*
 * Read it into PSRAM and hand it to feckless_abi_run(), on this task: the
 * app's app_main() runs here, with the main task's stack. It finds the
 * board up -- the panel, touch, the USB port with the card's and drive's
 * mass storage on it -- and initialises it again as it would at boot,
 * which feckless-abi v0.2.0's libraries allow. The console and storage
 * keep running under it.
 *
 * If the ELF will not load (a symbol this ABI lacks, another ABI
 * version), nothing of it has run and the menu comes back. If its
 * app_main() returns, its own tasks have the board: this task goes.
 */
static void run_elf(const entry_t *e)
{
    char path[800], size[24];
    snprintf(path, sizeof path, "%s/%s", s_dir, e->name);
    fmt_size(size, sizeof size, e->size);

    gfx_fill_rect(0, 0, W, H, C_BG);
    header("Run?", path);
    text_centred(H / 2 - 100, e->name, C_ELF);
    text_centred(H / 2 - 30, size, C_DIM);
    text_centred(H / 2 + 40, "Runs now, from the card. app2 is kept.", C_DIM);
    char abi[32];
    snprintf(abi, sizeof abi, "feckless ABI v%d", FECKLESS_ABI_VERSION);
    text_centred(H / 2 + 100, abi, C_DIM);
    const int bw = (W - 3 * PAD) / 2, by = H - FOOT_H + 20, bh = FOOT_H - 40;
    button(PAD, by, bw, bh, "Cancel", C_BTN);
    button(2 * PAD + bw, by, bw, bh, "Run", C_GO);
    show();
    int x, y;
    for (;;) {
        wait_tap(&x, &y, -1);
        if (hit(x, y, PAD, by, bw, bh)) return;
        if (hit(x, y, 2 * PAD + bw, by, bw, bh)) break;
    }

    gfx_fill_rect(0, 0, W, H, C_BG);
    header("Loading", path);
    show();
    uint8_t *image = NULL;
    size_t len = 0;
    char err[128];
    if (elfrun_load(path, &image, &len, draw_progress, (void *)e->name, err, sizeof err) != ESP_OK) {
        message("Not run", err, C_BAD);
        return;
    }

    ESP_LOGI(TAG, "running %s (%u bytes)", path, (unsigned)len);
    int ret = feckless_abi_run(image, len);
    if (ret == 0) {
        ESP_LOGI(TAG, "%s: app_main() returned; its tasks have the board", e->name);
        vTaskDelete(NULL);
    }
    heap_caps_free(image);
    snprintf(err, sizeof err, "It would not load (%d). The log names the symbol it lacked.", ret);
    message("Not run", err, C_BAD);
}

/* ---- the menu ------------------------------------------------------------ */

static void up_dir(void)
{
    char *slash = strrchr(s_dir, '/');
    if (!slash || slash == s_dir) s_dir[0] = '\0';   /* /sd -> the volumes */
    else *slash = '\0';
}

/* While the console is writing, say so -- what, where, how far -- and
 * nothing else. Taps are ignored: the computer is driving. */
static void show_xfer(void)
{
    xfer_t x;
    uint32_t g = xfer_get(&x), drawn = g - 1;
    int64_t last = 0;
    while (x.active) {
        int64_t now = esp_timer_get_time();
        if (g != drawn && now - last > 150 * 1000) {
            gfx_fill_rect(0, 0, W, H, C_BG);
            header(x.what, x.where);
            text_centred(H / 2 - 160, "From the computer on USB-C", C_DIM);
            text_centred(H / 2 - 90, x.name[0] ? x.name : "waiting for the sender...", C_BIN);
            const int y = H / 2;
            gfx_fill_rect(PAD, y, W - 2 * PAD, 40, C_BTN);
            if (x.total)
                gfx_fill_rect(PAD, y, (int)((W - 2 * PAD) * (int64_t)x.done / (int64_t)x.total), 40, C_GO);
            char s[64];
            if (x.total) snprintf(s, sizeof s, "%d%%  (%llu / %llu KB)", (int)(x.done * 100 / x.total),
                                  (unsigned long long)(x.done / 1024), (unsigned long long)(x.total / 1024));
            else snprintf(s, sizeof s, "%llu KB", (unsigned long long)(x.done / 1024));
            text_centred(y + 70, s, C_DIM);
            show();
            drawn = g;
            last = now;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
        g = xfer_get(&x);
    }
    touch_swallow();
}

static bool xfer_active(void)
{
    xfer_t x;
    xfer_get(&x);
    return x.active;
}

/* A tap, or false when a card or drive came or went, or the console
 * started writing. */
static bool tap_or_change(int *x, int *y, uint32_t *gen)
{
    for (;;) {
        if (wait_tap(x, y, 500)) return true;
        if (xfer_active()) return false;
        uint32_t g = storage_generation();
        if (g != *gen) { *gen = g; return false; }
    }
}

static void menu(void)
{
    int page = 0;
    bool rescan = true;
    uint32_t gen = 0;
    for (;;) {
        if (xfer_active()) { show_xfer(); rescan = true; }
        if (rescan) {
            gen = storage_generation();
            PHASE("scan %s ...", s_dir[0] ? s_dir : "(volumes)");
            scan();
            PHASE("scan done, %d entries", s_n);
            page = 0;
            rescan = false;
        }

        char app[64] = "";
        PHASE("read app2 header and NVS ...");
        bool have_app = install_present(app, sizeof app);
        PHASE("read done");
        /* row 0 is "boot app2" when there is one; the files follow */
        int extra = have_app ? 1 : 0;
        int pages = (s_n + extra + ROWS - 1) / ROWS;
        if (pages < 1) pages = 1;
        if (page >= pages) page = pages - 1;

        gfx_fill_rect(0, 0, W, H, C_BG);
        header(avoidant_title(), s_dir[0] ? s_dir : "microSD and USB");
        draw_gear();                    /* 0028 */
        for (int r = 0; r < ROWS; r++) {
            int i = page * ROWS + r - extra;
            int y = LIST_Y + r * ROW_H;
            if (i < -1 || (i == -1 && !have_app) || i >= s_n) break;
            if (i == -1) {
                gfx_fill_rect(0, y, W, ROW_H - 2, RGB(28, 52, 36));
                char s[96];
                snprintf(s, sizeof s, "Boot app2: %s", app);
                text(PAD, y + (ROW_H - TEXT_H) / 2, s, W - 2 * PAD, C_TEXT);
            } else {
                const entry_t *e = &s_ents[i];
                if (e->dir) {
                    char s[300];
                    if (!s_dir[0]) {
                        /* a volume: its label, "microSD" or "USB" */
                        char mp[8];
                        snprintf(mp, sizeof mp, "/%s", e->name);
                        snprintf(s, sizeof s, "%s", storage_label(storage_of_path(mp)));
                    } else {
                        snprintf(s, sizeof s, "%s/", e->name);
                    }
                    text(PAD, y + (ROW_H - TEXT_H) / 2, s, W - 2 * PAD, C_DIR);
                } else {
                    char sz[24];
                    fmt_size(sz, sizeof sz, e->size);
                    int sw = gfx_text_w(sz, SCALE);
                    text(PAD, y + (ROW_H - TEXT_H) / 2, e->name, W - 3 * PAD - sw,
                         is_elf(e->name) ? C_ELF : C_BIN);
                    text(W - PAD - sw, y + (ROW_H - TEXT_H) / 2, sz, sw, C_DIM);
                }
            }
            gfx_fill_rect(0, y + ROW_H - 2, W, 2, C_RULE);
        }
        if (s_n == 0)
            text(PAD, LIST_Y + (have_app ? ROW_H : 0) + 30,
                 s_dir[0] ? "No folders or apps here." : "No microSD card or USB drive.",
                 W - 2 * PAD, C_DIM);
        if (s_hidden && page == pages - 1) {
            char note[64];
            snprintf(note, sizeof note, "%d .bin file%s here %s not an ESP32-P4 app", s_hidden,
                     s_hidden == 1 ? "" : "s", s_hidden == 1 ? "is" : "are");
            int rows = s_n + extra - page * ROWS;     /* rows on this, the last, page */
            int ny = LIST_Y + rows * ROW_H + 30;
            if (s_n == 0) ny += TEXT_H + 30;            /* under "No folders or apps here." */
            if (ny + TEXT_H < H - FOOT_H) text(PAD, ny, note, W - 2 * PAD, C_DIM);
        }

        /* Up, Prev, Next, Rescan */
        const int bw = (W - 5 * PAD) / 4, by = H - FOOT_H + 20, bh = FOOT_H - 40;
        const char *labels[4] = { "Up", "Prev", "Next", "Rescan" };
        bool on[4] = { s_dir[0] != '\0', page > 0, page < pages - 1, true };
        for (int b = 0; b < 4; b++)
            button(PAD + b * (bw + PAD), by, bw, bh, labels[b], on[b] ? C_BTN : C_RULE);
        char pg[24];
        snprintf(pg, sizeof pg, "%d / %d", page + 1, pages);
        text(W - PAD - gfx_text_w(pg, SCALE), by - TEXT_H - 8, pg, W, C_DIM);
        PHASE("blit the menu ...");
        show();
        PHASE("blit done");

        int x, y;
        if (!tap_or_change(&x, &y, &gen)) { rescan = true; continue; }
        if (gear_hit(x, y)) { settings_page(); continue; }     /* 0028 */
        if (y >= by) {
            int b = (x - PAD) / (bw + PAD);
            if (b < 0 || b > 3 || !on[b]) continue;
            if (b == 0) { up_dir(); rescan = true; }
            if (b == 1) page--;
            if (b == 2) page++;
            if (b == 3) rescan = true;
            continue;
        }
        if (y < LIST_Y || y >= LIST_Y + ROWS * ROW_H) continue;
        int i = page * ROWS + (y - LIST_Y) / ROW_H - extra;
        if (i == -1 && have_app) {
            install_boot();
            message("Not booted", "app2 could not be marked to boot.", C_BAD);
        } else if (i >= 0 && i < s_n) {
            if (s_ents[i].dir) {
                size_t l = strlen(s_dir);   /* "" + "/sd", or "/sd" + "/bins" */
                if (l + 1 + strlen(s_ents[i].name) < sizeof s_dir) {
                    snprintf(s_dir + l, sizeof s_dir - l, "/%s", s_ents[i].name);
                    rescan = true;
                }
            } else if (is_elf(s_ents[i].name)) {
                run_elf(&s_ents[i]);
                rescan = true;
            } else {
                install_and_boot(&s_ents[i]);
                rescan = true;
            }
        }
    }
}

/* ---- restarting ---------------------------------------------------------- */

/*
 * On every esp_restart() -- booting app2, "reboot", the console's boot:
 * the panel off and the backlight down first.
 *
 * Between the reset and the next app's panel init (about 1.9 s for the
 * player) nothing drives the DSI link, and the panel shows cyan. Seen on
 * a Tab5 after cancelling autoboot and going on to boot app2 (0013's
 * markers: no "underrun happens" anywhere, so not the scanout starving).
 * DISPOFF holds the glass dark until the next app's init sends DISPON;
 * the backlight off covers the reset itself.
 */
static void blank_on_restart(void)
{
    PHASE("restart: panel off");
    lcd_backlight_set(0);
    if (lcd_panel()) esp_lcd_panel_disp_on_off(lcd_panel(), false);
}

/* ---- power-on ------------------------------------------------------------ */

/* app2 boots unless the screen is touched within AUTOBOOT_MS -- when a
 * reset brought us back out of it. When an app exited to us on purpose,
 * or app2 has never been booted, straight to the menu. */
static void autoboot(void)
{
    char app[64];
    if (!install_present(app, sizeof app)) return;
    if (!install_came_back_by_reset()) { PHASE("autoboot: not back by reset, to the menu"); return; }
    int64_t start = esp_timer_get_time();
    for (;;) {
        int left = AUTOBOOT_MS - (int)((esp_timer_get_time() - start) / 1000);
        if (left <= 0) break;
        gfx_fill_rect(0, 0, W, H, C_BG);
        header(avoidant_title(), NULL);
        text_centred(H / 2 - 120, "Booting", C_DIM);
        text_centred(H / 2 - 50, app, C_BIN);
        char s[32];
        snprintf(s, sizeof s, "in %d", (left + 999) / 1000);
        text_centred(H / 2 + 20, s, C_DIM);
        text_centred(H / 2 + 160, "Touch the screen for the menu", C_TEXT);
        show();
        int x, y;
        if (wait_tap(&x, &y, left < 250 ? left : 250)) { PHASE("autoboot: touched, to the menu"); return; }
        if (xfer_active()) return;      /* a computer is sending: stay */
    }
    install_boot();
}

void app_main(void)
{
    /*
     * 0026: the line to start reading a log from, as Lothesome's 0005
     * has it -- above is the ROM, the bootloader and IDF's startup, below
     * is this program -- with what it was built from, so a log can be
     * matched to a tree, and the ABI its .elf apps must be built for.
     * A warning, so the console's WARN level lets it through.
     */
    {
        const esp_app_desc_t *d = esp_app_get_description();
        ESP_LOGW(TAG, "=== Avoidant === %s, IDF %s, built %s %s, feckless ABI v%d",
                 d->version, d->idf_ver, d->date, d->time, FECKLESS_ABI_VERSION);
    }

    /* 0035: NVS through feckless_nvs_handler, which erases a partition
     * it cannot use, as this did -- and marks NVS up for the apps: the
     * ABI's nvs section hands them its calls but not nvsh_init(). */
    nvsh_init();

    ESP_ERROR_CHECK(tab5io_init());     /* bus, expanders, LCD_RST/TP_RST released */
    esp_lcd_panel_handle_t panel;
    ESP_ERROR_CHECK(lcd_init(&panel));
    ESP_ERROR_CHECK(gfx_init(panel, LCD_H_RES, LCD_V_RES));
    if (touch_init(tab5io_bus(), LCD_H_RES, LCD_V_RES) != ESP_OK)
        ESP_LOGE(TAG, "no touch controller found");
    gfx_fill_rect(0, 0, W, H, C_BG);
    show();
    lcd_backlight_set(BRIGHTNESS);
    esp_register_shutdown_handler(blank_on_restart);

    /* The USB host before storage: storage registers mass storage as a
     * class with it, which is refused once the port is up. Then the port,
     * which powers the USB-A socket. The card and the drive mount on
     * storage's own poll task, and remount as they come and go. */
    ESP_ERROR_CHECK(usbhost_init(tab5io_exp2()));
    storage_io_init();
    static const storage_usb_t usb = {
        .register_class = usbhost_register_class,
        .set_power      = usbhost_set_power,
        .powered        = usbhost_powered,
    };
    ESP_ERROR_CHECK(storage_init(&usb));
    /* 0043: the network, as storage: this firmware brings it up for the
     * apps it runs (feckless-libs' 6005) -- the saved networks, Defeatist's
     * list, which Errant reads too; USB Ethernet's class, before the port
     * comes up, with the configuration selector 0042 set here, so a
     * Realtek adapter enumerates in CDC-ECM; the radio's task. Nothing is
     * powered: an app sets its policy and asks for the radio. */
    {
        const feckless_net_start_t net = {
            .exp2                  = tab5io_exp2(),
            .usb_register_class    = usbhost_register_class,
            .usb_set_config_select = usbhost_set_config_select,
            .nvs_ns                = "defeatist",
        };
        feckless_net_start(&net);
    }
    usbhost_start();

    if (console_start() != ESP_OK) ESP_LOGE(TAG, "no serial console");

    PHASE("autoboot");
    autoboot();
    PHASE("menu");
    menu();
}
