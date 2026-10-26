/*
 * Duplicitous for the M5Stack Tab5 -- choose a .bin on the microSD card,
 * or a USB drive, write it to app2 and boot it, in the manner of
 * bmorcelli/Launcher.
 *
 * At power-on, if app2 holds an app, it boots after a short countdown
 * unless the screen is touched. Otherwise, or after a touch, the card and
 * the drive are listed, then their folders and .bin files; tapping a .bin asks, writes and
 * boots it. The console on the USB-C port (console.h) does the same
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

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "storage.h"
#include "storage_io.h"
#include "usbhost.h"

#include "gfx.h"
#include "lcd.h"
#include "tab5io.h"
#include "touch.h"

#include "console.h"
#include "install.h"
#include "vol.h"
#include "xfer.h"

static const char *TAG = "duplicitous";

#define AUTOBOOT_MS     3000
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

/* ---- the card listing --------------------------------------------------- */

typedef struct {
    char *name;
    bool dir;
    long size;
} entry_t;

static entry_t *s_ents;
static int s_n;
/* "" is the list of volumes; otherwise a directory on one. */
static char s_dir[512] = "";

static bool is_bin(const char *n)
{
    size_t l = strlen(n);
    return l > 4 && !strcasecmp(n + l - 4, ".bin");
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
            if (e->d_name[0] == '.' || (!dir && !is_bin(e->d_name))) continue;
            if (!strcasecmp(e->d_name, "System Volume Information")) continue;
            long size = -1;
            if (!dir) {
                char full[800];
                struct stat st;
                snprintf(full, sizeof full, "%s/%s", s_dir, e->d_name);
                if (stat(full, &st) == 0) size = (long)st.st_size;
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
        if (rescan) { gen = storage_generation(); scan(); page = 0; rescan = false; }

        char app[64] = "";
        bool have_app = install_present(app, sizeof app);
        /* row 0 is "boot app2" when there is one; the files follow */
        int extra = have_app ? 1 : 0;
        int pages = (s_n + extra + ROWS - 1) / ROWS;
        if (pages < 1) pages = 1;
        if (page >= pages) page = pages - 1;

        gfx_fill_rect(0, 0, W, H, C_BG);
        header(duplicitous_title(), s_dir[0] ? s_dir : "microSD and USB");
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
                    text(PAD, y + (ROW_H - TEXT_H) / 2, e->name, W - 3 * PAD - sw, C_BIN);
                    text(W - PAD - sw, y + (ROW_H - TEXT_H) / 2, sz, sw, C_DIM);
                }
            }
            gfx_fill_rect(0, y + ROW_H - 2, W, 2, C_RULE);
        }
        if (s_n == 0)
            text(PAD, LIST_Y + (have_app ? ROW_H : 0) + 30,
                 s_dir[0] ? "No folders or .bin files here." : "No microSD card or USB drive.",
                 W - 2 * PAD, C_DIM);

        /* Up, Prev, Next, Rescan */
        const int bw = (W - 5 * PAD) / 4, by = H - FOOT_H + 20, bh = FOOT_H - 40;
        const char *labels[4] = { "Up", "Prev", "Next", "Rescan" };
        bool on[4] = { s_dir[0] != '\0', page > 0, page < pages - 1, true };
        for (int b = 0; b < 4; b++)
            button(PAD + b * (bw + PAD), by, bw, bh, labels[b], on[b] ? C_BTN : C_RULE);
        char pg[24];
        snprintf(pg, sizeof pg, "%d / %d", page + 1, pages);
        text(W - PAD - gfx_text_w(pg, SCALE), by - TEXT_H - 8, pg, W, C_DIM);
        show();

        int x, y;
        if (!tap_or_change(&x, &y, &gen)) { rescan = true; continue; }
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
            } else {
                install_and_boot(&s_ents[i]);
                rescan = true;
            }
        }
    }
}

/* ---- power-on ------------------------------------------------------------ */

/* app2 boots unless the screen is touched within AUTOBOOT_MS. */
static void autoboot(void)
{
    char app[64];
    if (!install_present(app, sizeof app)) return;
    int64_t start = esp_timer_get_time();
    for (;;) {
        int left = AUTOBOOT_MS - (int)((esp_timer_get_time() - start) / 1000);
        if (left <= 0) break;
        gfx_fill_rect(0, 0, W, H, C_BG);
        header(duplicitous_title(), NULL);
        text_centred(H / 2 - 120, "Booting", C_DIM);
        text_centred(H / 2 - 50, app, C_BIN);
        char s[32];
        snprintf(s, sizeof s, "in %d", (left + 999) / 1000);
        text_centred(H / 2 + 20, s, C_DIM);
        text_centred(H / 2 + 160, "Touch the screen for the menu", C_TEXT);
        show();
        int x, y;
        if (wait_tap(&x, &y, left < 250 ? left : 250)) return;
        if (xfer_active()) return;      /* a computer is sending: stay */
    }
    install_boot();
}

void app_main(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    ESP_ERROR_CHECK(tab5io_init());     /* bus, expanders, LCD_RST/TP_RST released */
    esp_lcd_panel_handle_t panel;
    ESP_ERROR_CHECK(lcd_init(&panel));
    ESP_ERROR_CHECK(gfx_init(panel, LCD_H_RES, LCD_V_RES));
    if (touch_init(tab5io_bus(), LCD_H_RES, LCD_V_RES) != ESP_OK)
        ESP_LOGE(TAG, "no touch controller found");
    gfx_fill_rect(0, 0, W, H, C_BG);
    show();
    lcd_backlight_set(BRIGHTNESS);

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
    usbhost_start();

    if (console_start() != ESP_OK) ESP_LOGE(TAG, "no serial console");

    autoboot();
    menu();
}
