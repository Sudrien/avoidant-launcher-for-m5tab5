/*
 * wifiui.c -- see wifiui.h.
 *
 * Three screens. The saved list (wifiui_page), where a network is
 * forgotten or one added; the form (add_form), Defeatist's setup page on
 * the glass: Network, a box that opens the scan's list as the page's
 * <select> does, a name field under it once "Other or hidden network" is
 * chosen, Password, and the keyboard; and the list itself (pick_network).
 *
 * A scan, the radio's start and a join each take seconds -- a join up to
 * two timeouts, about thirty. They run on a short task of their own while
 * this one, the screen's only writer, turns a spinner; it waits for them,
 * so nothing else is drawn meanwhile.
 *
 * SPDX-License-Identifier: MIT
 */
#include "wifiui.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "feckless_keyboard.h"
#include "gfx.h"
#include "lcd.h"
#include "touch.h"
#include "wifi.h"
#include "wifistore.h"

#include "limport.h"
#include "limport_core.h"    /* li_classify() */

static const char *TAG = "wifiui";

/* main.c's layout and colours, portrait 720 x 1280. */
#define W           LCD_H_RES
#define H           LCD_V_RES
#define PAD         24
#define SCALE       3
#define SSCALE      2
#define TEXT_H      GFX_GLYPH_H(SCALE)
#define STEXT_H     GFX_GLYPH_H(SSCALE)
#define HEAD_H      140
#define ROW_H       88
#define FOOT_H      130
#define FOOT_Y      (H - FOOT_H + 20)
#define FOOT_BH     (FOOT_H - 40)

#define C_BG        RGB(16, 18, 24)
#define C_HEAD      RGB(32, 36, 48)
#define C_TEXT      RGB(235, 235, 235)
#define C_DIM       RGB(140, 145, 160)
#define C_DIR       RGB(120, 190, 255)
#define C_RULE      RGB(44, 48, 60)
#define C_BTN       RGB(52, 58, 76)
#define C_FIELD     RGB(26, 29, 38)
#define C_GO        RGB(40, 130, 70)
#define C_BAD       RGB(220, 90, 80)
#define C_ON        RGB(120, 96, 40)

#define SEEN_MAX    32

/* ---- drawing and taps, as main.c's -------------------------------------- */

static bool wait_tap(int *x, int *y)
{
    bool down = false;
    *x = *y = -1;
    for (;;) {
        int tx, ty;
        bool now = touch_get(&tx, &ty);
        if (now && !down) { down = true; *x = tx; *y = ty; }
        if (!now && down) return true;
        vTaskDelay(pdMS_TO_TICKS(15));
    }
}

static bool hit(int x, int y, int bx, int by, int bw, int bh)
{
    return x >= bx && x < bx + bw && y >= by && y < by + bh;
}

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
    text(x + (tw < w ? (w - tw) / 2 : 8), y + (h - TEXT_H) / 2, label, w - 16, C_TEXT);
}

static void header(const char *title, const char *sub)
{
    gfx_fill_rect(0, 0, W, HEAD_H, C_HEAD);
    text(PAD, 28, title, W - 2 * PAD, C_TEXT);
    if (sub) gfx_draw_text_tail(PAD, 28 + TEXT_H + 18, sub, SCALE, W - 2 * PAD, C_DIM);
}

static void show(void) { gfx_blit(0, H); }

static void two_buttons(const char *a, uint16_t ca, const char *b, uint16_t cb)
{
    const int bw = (W - 3 * PAD) / 2;
    button(PAD, FOOT_Y, bw, FOOT_BH, a, ca);
    button(2 * PAD + bw, FOOT_Y, bw, FOOT_BH, b, cb);
}

/* 0 for the left footer button, 1 for the right, -1 for neither. */
static int two_hit(int x, int y)
{
    const int bw = (W - 3 * PAD) / 2;
    if (hit(x, y, PAD, FOOT_Y, bw, FOOT_BH)) return 0;
    if (hit(x, y, 2 * PAD + bw, FOOT_Y, bw, FOOT_BH)) return 1;
    return -1;
}

static void message(const char *title, const char *body, uint16_t c)
{
    gfx_fill_rect(0, 0, W, H, C_BG);
    header(title, NULL);
    gfx_draw_para(PAD, HEAD_H + 40, body, SCALE, W - 2 * PAD, TEXT_H + 10, 20, c);
    button(PAD, FOOT_Y, W - 2 * PAD, FOOT_BH, "OK", C_BTN);
    show();
    int x, y;
    do wait_tap(&x, &y); while (!hit(x, y, 0, H - FOOT_H, W, FOOT_H));
}

/* A question, and two answers: true for the right-hand one. */
static bool ask(const char *title, const char *body, const char *no, const char *yes, uint16_t cyes)
{
    gfx_fill_rect(0, 0, W, H, C_BG);
    header(title, NULL);
    gfx_draw_para(PAD, HEAD_H + 40, body, SCALE, W - 2 * PAD, TEXT_H + 10, 20, C_TEXT);
    two_buttons(no, C_BTN, yes, cyes);
    show();
    for (;;) {
        int x, y, b;
        wait_tap(&x, &y);
        if ((b = two_hit(x, y)) >= 0) return b == 1;
    }
}

/* ---- the slow calls, on a task of their own ----------------------------- */

typedef struct {
    int (*fn)(void *arg);
    void *arg;
    int ret;
    SemaphoreHandle_t done;
} job_t;

static void job_task(void *p)
{
    job_t *j = p;
    j->ret = j->fn(j->arg);
    xSemaphoreGive(j->done);
    vTaskDelete(NULL);
}

/* fn(arg) on another task, a spinner and two lines here until it returns.
 * Its return, or `fail` if the task could not be had. */
static int busy(int (*fn)(void *), void *arg, const char *title, const char *line,
                const char *sub, int fail)
{
    job_t j = { .fn = fn, .arg = arg, .ret = fail, .done = xSemaphoreCreateBinary() };
    if (!j.done) return fail;
    /* 8 KB: the PBKDF2 behind a join's derived key, and the driver's calls. */
    if (xTaskCreate(job_task, "wifiui", 8192, &j, 5, NULL) != pdPASS) {
        vSemaphoreDelete(j.done);
        return fail;
    }
    gfx_fill_rect(0, 0, W, H, C_BG);
    header(title, NULL);
    text_centred(H / 2 - 180, line, C_TEXT);
    if (sub) text_centred(H / 2 - 110, sub, C_DIM);
    show();
    const int64_t t0 = esp_timer_get_time();
    while (xSemaphoreTake(j.done, pdMS_TO_TICKS(60)) != pdTRUE) {
        const uint32_t ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
        gfx_fill_rect(W / 2 - 60, H / 2 - 20, 120, 120, C_BG);
        gfx_draw_spinner(W / 2, H / 2 + 40, 50, ms, C_DIR, C_BTN);
        gfx_blit(H / 2 - 20, H / 2 + 100);
    }
    vSemaphoreDelete(j.done);
    touch_swallow();
    return j.ret;
}

/* ---- the radio ---------------------------------------------------------- */

static wifi_seen_t s_seen[SEEN_MAX];
static int s_seen_n = -1;       /* -1: not scanned since the page opened */
static int s_hidden_n;

static int start_fn(void *arg)
{
    (void)arg;
    return wifi_start() == ESP_OK;
}

static int stop_fn(void *arg)
{
    (void)arg;
    return wifi_stop() == ESP_OK;
}

static int scan_fn(void *arg)
{
    (void)arg;
    return wifi_scan_list(s_seen, SEEN_MAX);
}

/* Up if it is not, then a scan. The list keeps one entry a name -- the
 * strongest, as the driver sorts them -- and counts the hidden ones. */
static bool rescan(void)
{
    if (!wifi_up() && !busy(start_fn, NULL, "Wi-Fi", "Starting the radio...", NULL, 0)) {
        message("Wi-Fi", "The radio did not come up. The log says why.", C_BAD);
        return false;
    }
    int n = busy(scan_fn, NULL, "Wi-Fi", "Listening for networks...", "About four seconds.", -1);
    if (n < 0) {
        s_seen_n = 0;
        message("Wi-Fi", "The scan failed. The log says why.", C_BAD);
        return false;
    }
    int k = 0;
    s_hidden_n = 0;
    for (int i = 0; i < n; i++) {
        if (s_seen[i].hidden || !s_seen[i].ssid[0]) { s_hidden_n++; continue; }
        bool dup = false;
        for (int j = 0; j < k && !dup; j++) dup = !strcmp(s_seen[j].ssid, s_seen[i].ssid);
        if (!dup) s_seen[k++] = s_seen[i];
    }
    s_seen_n = k;
    return true;
}

static bool is_saved(const char *ssid)
{
    wifistore_cred_t c;
    for (int i = 0; i < wifistore_count(); i++)
        if (wifistore_get(i, &c) && !strcmp(c.ssid, ssid)) {
            memset(c.secret, 0, sizeof c.secret);
            return true;
        }
    return false;
}

static const wifi_seen_t *seen(const char *ssid)
{
    for (int i = 0; i < s_seen_n; i++)
        if (!strcmp(s_seen[i].ssid, ssid)) return &s_seen[i];
    return NULL;
}

/* ---- joining: Defeatist's wifijoin_try(), its order and its saving ------ */

typedef struct {
    char ssid[WIFISTORE_SSID_MAX + 1];
    char pass[WIFISTORE_SECRET_MAX + 1];
    bool wpa3;          /* the scan heard it as WPA3 or WPA2/WPA3 */
    bool is_psk;        /* typed as 64 hex digits */
} join_t;

static int join_fn(void *arg)
{
    join_t *j = arg;
    esp_err_t err;
    const char *stored = j->pass;
    bool psk = j->is_psk;
    char hex[WIFISTORE_SECRET_MAX + 1] = { 0 };

    if (j->is_psk || j->wpa3) {
        /* a key as typed; or an SAE network, which no derived key joins */
        ESP_LOGI(TAG, "trying %.32s (%s)", j->ssid, j->is_psk ? "PSK as typed" : "passphrase: WPA3-capable");
        err = wifi_join(j->ssid, j->pass, WIFI_JOIN_TIMEOUT_MS);
    } else {
        /* WPA2, or not heard: the derived key first, so a WPA2 network
         * keeps a key and not the passphrase; the passphrase if refused */
        ESP_LOGI(TAG, "trying %.32s (PSK, then passphrase)", j->ssid);
        uint8_t key[32];
        err = ESP_ERR_WIFI_PASSWORD;
        if (wifi_psk(j->ssid, j->pass, key) == ESP_OK) {
            static const char d[] = "0123456789abcdef";
            for (int i = 0; i < 32; i++) { hex[2 * i] = d[key[i] >> 4]; hex[2 * i + 1] = d[key[i] & 15]; }
            err = wifi_join(j->ssid, hex, WIFI_JOIN_TIMEOUT_MS);
            stored = hex;
            psk = true;
        }
        memset(key, 0, sizeof key);
        if (err == ESP_ERR_WIFI_PASSWORD) {
            err = wifi_join(j->ssid, j->pass, WIFI_JOIN_TIMEOUT_MS);
            stored = j->pass;
            psk = false;
        }
    }
    if (err == ESP_OK) {
        err = wifistore_save(j->ssid, stored, psk);
        if (err == ESP_OK) ESP_LOGI(TAG, "saved %.32s as %s", j->ssid, psk ? "PSK" : "passphrase");
        else ESP_LOGE(TAG, "joined but could not save: %s", esp_err_to_name(err));
    }
    memset(hex, 0, sizeof hex);
    memset(j->pass, 0, sizeof j->pass);
    return (int)err;
}

/* ---- the scan's list, as the page's <select> ---------------------------- */

#define PICK_OTHER  (-2)
#define PICK_BACK   (-1)

/* An index into s_seen, PICK_OTHER, or PICK_BACK. */
static int pick_network(void)
{
    const int rows = (H - HEAD_H - FOOT_H) / ROW_H;
    int page = 0;
    for (;;) {
        /* row 0 is "Other or hidden network", as the page's last option */
        const int n = s_seen_n + 1;
        const int pages = (n + rows - 1) / rows;
        if (page >= pages) page = pages - 1;

        char sub[64];
        snprintf(sub, sizeof sub, "%d heard, strongest first", s_seen_n);
        gfx_fill_rect(0, 0, W, H, C_BG);
        header("Choose a network", sub);
        for (int r = 0; r < rows; r++) {
            const int i = page * rows + r - 1;
            const int y = HEAD_H + r * ROW_H;
            if (i >= s_seen_n) break;
            const int ty = y + (ROW_H - TEXT_H) / 2;
            if (i < 0) {
                text(PAD, ty, s_hidden_n ? "Other or hidden network (hidden nearby)"
                                         : "Other or hidden network", W - 2 * PAD, C_DIR);
            } else {
                const wifi_seen_t *s = &s_seen[i];
                const bool open = s->auth == WIFI_AUTH_OPEN;
                char meta[48];
                snprintf(meta, sizeof meta, "%s%s %d", is_saved(s->ssid) ? "saved, " : "",
                         open ? "open" : wifi_auth_name(s->auth), s->rssi);
                const int mw = gfx_text_w(meta, SSCALE);
                text(PAD, ty, s->ssid, W - 3 * PAD - mw, open ? C_DIM : C_TEXT);
                gfx_draw_text(W - PAD - mw, y + (ROW_H - STEXT_H) / 2, meta, SSCALE, mw, C_DIM);
            }
            gfx_fill_rect(0, y + ROW_H - 2, W, 2, C_RULE);
        }
        const int bw = (W - 5 * PAD) / 4;
        const char *labels[4] = { "Back", "Prev", "Next", "Rescan" };
        const bool on[4] = { true, page > 0, page < pages - 1, true };
        for (int b = 0; b < 4; b++)
            button(PAD + b * (bw + PAD), FOOT_Y, bw, FOOT_BH, labels[b], on[b] ? C_BTN : C_RULE);
        show();

        int x, y;
        wait_tap(&x, &y);
        if (y >= FOOT_Y) {
            const int b = (x - PAD) / (bw + PAD);
            if (b < 0 || b > 3 || !on[b]) continue;
            if (b == 0) return PICK_BACK;
            if (b == 1) page--;
            if (b == 2) page++;
            if (b == 3) { rescan(); page = 0; }
            continue;
        }
        if (y < HEAD_H || y >= HEAD_H + rows * ROW_H) continue;
        const int i = page * rows + (y - HEAD_H) / ROW_H - 1;
        if (i < 0) return PICK_OTHER;
        if (i >= s_seen_n) continue;
        if (s_seen[i].auth == WIFI_AUTH_OPEN) {
            message("Open network", "Networks with no password are not kept: "
                    "every app would join them without asking.", C_TEXT);
            continue;
        }
        return i;
    }
}

/* ---- the form ------------------------------------------------------------ */

#define LBL_NET_Y   (HEAD_H + 20)
#define BOX_H       84
#define NET_Y       (LBL_NET_Y + TEXT_H + 10)
#define LBL_OTH_Y   (NET_Y + BOX_H + 18)
#define OTH_Y       (LBL_OTH_Y + TEXT_H + 10)
#define LBL_PW_Y    (OTH_Y + BOX_H + 18)
#define PW_Y        (LBL_PW_Y + TEXT_H + 10)
#define NOTE_Y      (PW_Y + BOX_H + 14)
#define SHOW_W      150
#define KEY_H       80
#define SPACE_H     76
#define SPACE_Y     (FOOT_Y - 20 - SPACE_H)
#define KBD_Y       (SPACE_Y - 10 - KBD_ROWS * KEY_H)

enum { F_OTHER, F_PASS };

/* A text field; the focused one has a caret and a lit edge. */
static void field(int y, int w, const char *s, bool masked, bool focus, const char *placeholder)
{
    gfx_fill_rect(PAD, y, w, BOX_H, focus ? C_GO : C_RULE);
    gfx_fill_rect(PAD + 3, y + 3, w - 6, BOX_H - 6, C_FIELD);
    char shown[WIFISTORE_SECRET_MAX + 2];
    size_t n = strlen(s);
    if (masked) { memset(shown, '*', n); shown[n] = '\0'; }
    else snprintf(shown, sizeof shown, "%s", s);
    if (focus) strcat(shown, "_");
    const int ty = y + (BOX_H - TEXT_H) / 2;
    if (!n && !focus && placeholder) text(PAD + 16, ty, placeholder, w - 32, C_DIM);
    else gfx_draw_text_tail(PAD + 16, ty, shown, SCALE, w - 32, C_TEXT);
}

/* The network box: what is chosen, and a triangle that says it opens. */
static void dropdown(int y, const char *s, bool chosen)
{
    const int w = W - 2 * PAD;
    gfx_fill_rect(PAD, y, w, BOX_H, C_BTN);
    text(PAD + 16, y + (BOX_H - TEXT_H) / 2, s, w - 90, chosen ? C_TEXT : C_DIM);
    const int cx = PAD + w - 40, cy = y + BOX_H / 2;
    const int tri[6] = { cx - 16, cy - 8, cx + 16, cy - 8, cx, cy + 12 };
    gfx_fill_poly(tri, 3, C_TEXT);
}

/*
 * Add one network. True when one joined and was saved -- the caller
 * offers another. The form keeps what was typed when a join fails, so a
 * wrong password is a correction, not a new start; the password itself
 * is cleared on the way out either way.
 */
static bool add_form(void)
{
    if (s_seen_n < 0) rescan();

    int pick;
    char other[WIFISTORE_SSID_MAX + 1] = "";
    char pass[WIFISTORE_SECRET_MAX + 1] = "";
    bool reveal = false;
    int focus = F_PASS;
    char note[160] = "";
    uint16_t note_c = C_DIM;
    kbd_t kb = { .x = 0, .y = KBD_Y, .w = W, .key_h = KEY_H, .scale = SCALE,
                 .key = C_BTN, .text = C_TEXT, .on = C_ON };
    bool ok = false;

    /* the list first, as the page opens on its <select> */
    pick = pick_network();
    if (pick == PICK_BACK) return false;
    if (pick == PICK_OTHER) focus = F_OTHER;

    for (;;) {
        const bool typed = pick == PICK_OTHER;
        if (!typed && focus == F_OTHER) focus = F_PASS;
        char sub[64];
        snprintf(sub, sizeof sub, "%d of %d saved; tried first",
                 wifistore_count(), WIFISTORE_MAX);
        gfx_fill_rect(0, 0, W, H, C_BG);
        header("Add a network", sub);

        text(PAD, LBL_NET_Y, "Network", W - 2 * PAD, C_TEXT);
        if (typed) {
            dropdown(NET_Y, "Other or hidden network", true);
            text(PAD, LBL_OTH_Y, "Network name", W - 2 * PAD, C_TEXT);
            field(OTH_Y, W - 2 * PAD, other, false, focus == F_OTHER, "as the router spells it");
        } else {
            const wifi_seen_t *s = &s_seen[pick];
            char line[96];
            snprintf(line, sizeof line, "%s", s->ssid);
            dropdown(NET_Y, line, true);
            snprintf(line, sizeof line, "%s, %d dBm, channel %u%s", wifi_auth_name(s->auth), s->rssi,
                     (unsigned)s->channel, is_saved(s->ssid) ? " -- saved; this replaces it" : "");
            gfx_draw_text(PAD, LBL_OTH_Y + 10, line, SSCALE, W - 2 * PAD, C_DIM);
        }

        text(PAD, LBL_PW_Y, "Password", W - 2 * PAD, C_TEXT);
        field(PW_Y, W - 3 * PAD - SHOW_W, pass, !reveal, focus == F_PASS, NULL);
        button(W - PAD - SHOW_W, PW_Y, SHOW_W, BOX_H, reveal ? "Hide" : "Show", reveal ? C_ON : C_BTN);
        if (note[0])
            gfx_draw_para(PAD, NOTE_Y, note, SSCALE, W - 2 * PAD, STEXT_H + 6, 3, note_c);

        kbd_draw(&kb);
        button(W / 4, SPACE_Y, W / 2, SPACE_H, "space", C_BTN);
        two_buttons("Cancel", C_BTN, "Join and save", C_GO);
        show();

        int x, y;
        wait_tap(&x, &y);
        char *buf = focus == F_OTHER ? other : pass;
        const size_t cap = focus == F_OTHER ? sizeof other : sizeof pass;

        int k = kbd_hit(&kb, x, y);
        if (k != KBD_NONE) {
            /* an SSID is up to 32 bytes; a secret up to 64 */
            if (k > 0 && strlen(buf) + 1 >= cap) continue;
            kbd_edit(buf, cap, k);
            continue;
        }
        if (hit(x, y, W / 4, SPACE_Y, W / 2, SPACE_H)) {
            if (strlen(buf) + 1 < cap) kbd_edit(buf, cap, ' ');
            continue;
        }
        if (hit(x, y, PAD, NET_Y, W - 2 * PAD, BOX_H)) {
            const int p = pick_network();
            if (p != PICK_BACK) {
                pick = p;
                focus = p == PICK_OTHER ? F_OTHER : F_PASS;
                note[0] = '\0';
            }
            continue;
        }
        if (typed && hit(x, y, PAD, OTH_Y, W - 2 * PAD, BOX_H)) { focus = F_OTHER; continue; }
        if (hit(x, y, PAD, PW_Y, W - 3 * PAD - SHOW_W, BOX_H)) { focus = F_PASS; continue; }
        if (hit(x, y, W - PAD - SHOW_W, PW_Y, SHOW_W, BOX_H)) { reveal = !reveal; continue; }

        const int b = two_hit(x, y);
        if (b == 0) break;
        if (b != 1) continue;

        /* Join and save: the checks the page makes, before any radio */
        join_t j = { 0 };
        snprintf(j.ssid, sizeof j.ssid, "%s", typed ? other : s_seen[pick].ssid);
        note_c = C_BAD;
        if (!j.ssid[0]) {
            snprintf(note, sizeof note, "Type the network's name.");
            focus = F_OTHER;
            continue;
        }
        if (!pass[0]) {
            snprintf(note, sizeof note, "Networks with no password are not kept.");
            continue;
        }
        if (!li_classify(pass, &j.is_psk)) {
            snprintf(note, sizeof note, "A password is 8 to 63 characters, or a key of exactly "
                     "64 hex digits. This is %u.", (unsigned)strlen(pass));
            continue;
        }
        snprintf(j.pass, sizeof j.pass, "%s", pass);
        const wifi_seen_t *s = seen(j.ssid);
        j.wpa3 = s && (s->auth == WIFI_AUTH_WPA3_PSK || s->auth == WIFI_AUTH_WPA2_WPA3_PSK);

        if (!wifi_up() && !busy(start_fn, NULL, "Wi-Fi", "Starting the radio...", NULL, 0)) {
            snprintf(note, sizeof note, "The radio did not come up. The log says why.");
            memset(&j, 0, sizeof j);
            continue;
        }
        char line[64];
        snprintf(line, sizeof line, "Joining %s...", j.ssid);
        const esp_err_t err = (esp_err_t)busy(join_fn, &j, "Add a network", line,
                                              "Up to half a minute.", ESP_ERR_NO_MEM);
        memset(&j.pass, 0, sizeof j.pass);
        if (err == ESP_OK) { ok = true; break; }
        if (err == ESP_ERR_NOT_FOUND)
            snprintf(note, sizeof note, "%.32s was not heard. Is the name right, and is it in range?", j.ssid);
        else if (err == ESP_ERR_WIFI_PASSWORD)
            snprintf(note, sizeof note, "%.32s refused the password.", j.ssid);
        else if (err == ESP_ERR_TIMEOUT)
            snprintf(note, sizeof note, "No answer from %.32s. Try again, nearer it.", j.ssid);
        else
            snprintf(note, sizeof note, "Not saved: %s.", esp_err_to_name(err));
        focus = F_PASS;
    }

    char name[WIFISTORE_SSID_MAX + 1];
    snprintf(name, sizeof name, "%s", pick == PICK_OTHER ? other : (pick >= 0 ? s_seen[pick].ssid : ""));
    memset(pass, 0, sizeof pass);
    if (!ok) return false;
    char body[160];
    snprintf(body, sizeof body, "Joined and saved %s. Apps that use the network list have it now.", name);
    return ask("Saved", body, "Done", "Add another", C_GO);
}

/* ---- the saved list ------------------------------------------------------ */

/* 0045's, moved here from the gear's page. */
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

#define SROW_H      84
#define FORGET_W    170
#define LIST_Y      (HEAD_H + 20)

void wifiui_page(void)
{
    const bool was_up = wifi_up();
    s_seen_n = -1;
    const int add_y = LIST_Y + WIFISTORE_MAX * SROW_H + 30, add_h = 90;
    const int imp_y = add_y + add_h + 20;
    for (;;) {
        const int n = wifistore_count();
        char sub[64];
        snprintf(sub, sizeof sub, "%d of %d saved, for every app", n, WIFISTORE_MAX);
        gfx_fill_rect(0, 0, W, H, C_BG);
        header("Wi-Fi networks", sub);

        char joined[WIFISTORE_SSID_MAX + 1] = "";
        wifi_sta_ssid(joined, sizeof joined);
        for (int i = 0; i < n; i++) {
            wifistore_cred_t c;
            if (!wifistore_get(i, &c)) break;
            const int y = LIST_Y + i * SROW_H;
            const char *kind = c.is_psk ? "key" : "passphrase";
            const char *state = !strcmp(c.ssid, joined) ? "joined, " : "";
            char meta[32];
            snprintf(meta, sizeof meta, "%s%s", state, kind);
            const int mw = gfx_text_w(meta, SSCALE);
            text(PAD, y + (SROW_H - TEXT_H) / 2, c.ssid, W - 4 * PAD - FORGET_W - mw, C_TEXT);
            gfx_draw_text(W - 2 * PAD - FORGET_W - mw, y + (SROW_H - STEXT_H) / 2, meta, SSCALE, mw, C_DIM);
            button(W - PAD - FORGET_W, y + 8, FORGET_W, SROW_H - 16, "Forget", C_BTN);
            gfx_fill_rect(0, y + SROW_H - 2, W, 2, C_RULE);
            memset(c.secret, 0, sizeof c.secret);
        }
        if (!n)
            text(PAD, LIST_Y + 20, "None yet.", W - 2 * PAD, C_DIM);

        button(PAD, add_y, W - 2 * PAD, add_h, "Add a network", C_GO);
        button(PAD, imp_y, W - 2 * PAD, add_h, "Import from M5Launcher", C_BTN);
        int ny = imp_y + add_h + 16;
        if (n >= WIFISTORE_MAX) {
            gfx_draw_text(PAD, ny, "The list is full: adding one drops the oldest.",
                          SSCALE, W - 2 * PAD, C_DIM);
            ny += STEXT_H + 10;
        }
        gfx_draw_text(PAD, ny, "Import: config.conf and its M5Launcher .bin.",
                      SSCALE, W - 2 * PAD, C_DIM);
        button(PAD, FOOT_Y, W - 2 * PAD, FOOT_BH, "Back", C_BTN);
        show();

        int x, y;
        wait_tap(&x, &y);
        if (hit(x, y, 0, H - FOOT_H, W, FOOT_H)) break;
        if (hit(x, y, PAD, add_y, W - 2 * PAD, add_h)) { while (add_form()) {} continue; }
        if (hit(x, y, PAD, imp_y, W - 2 * PAD, add_h)) { import_wifi(); continue; }
        for (int i = 0; i < n; i++) {
            if (!hit(x, y, W - PAD - FORGET_W, LIST_Y + i * SROW_H, FORGET_W, SROW_H)) continue;
            wifistore_cred_t c;
            if (!wifistore_get(i, &c)) break;
            memset(c.secret, 0, sizeof c.secret);
            char body[120];
            snprintf(body, sizeof body, "Forget %s? Avoidant and its apps stop joining it.", c.ssid);
            if (ask("Forget", body, "Keep", "Forget", C_BAD)) {
                wifistore_forget(c.ssid);
                ESP_LOGI(TAG, "forgot %.32s", c.ssid);
            }
            break;
        }
    }
    /* as it was: an app that wants the radio asks for it */
    if (!was_up && wifi_up())
        busy(stop_fn, NULL, "Wi-Fi", "Stopping the radio...", NULL, 0);
    s_seen_n = -1;
}
