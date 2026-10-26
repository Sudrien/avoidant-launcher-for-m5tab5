/*
 * console.c -- see console.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "console.h"

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "install.h"
#include "sha256.h"
#include "vol.h"
#include "xfer.h"
#include "ymodem.h"

static const char *TAG = "console";

#define LINE_MAX_LEN  1024

const char *duplicitous_title(void)
{
    static char t[48];
    if (!t[0]) snprintf(t, sizeof t, "Duplicitous %s", esp_app_get_description()->version);
    return t;
}

static void reply(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    fflush(stdout);
}

/* The volume's own root: /sd or /usb itself. */
static bool is_root(const char *path)
{
    storage_id_t id = storage_of_path(path);
    return id != STORAGE_COUNT && !strcmp(path, storage_mount_path(id));
}

/* mkdir -p for the directories above path (on a volume). */
static void make_parents(const char *path)
{
    char tmp[LINE_MAX_LEN];
    snprintf(tmp, sizeof tmp, "%s", path);
    char *p = strchr(tmp + 1, '/');        /* past /sd or /usb */
    for (p = p ? p + 1 : tmp + strlen(tmp); *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        mkdir(tmp, 0775);
        *p = '/';
    }
}

static void cmd_ls(const char *arg)
{
    if (!*arg || !strcmp(arg, "/")) {
        /* the volumes themselves */
        for (int i = 0; i < STORAGE_COUNT; i++)
            if (storage_present((storage_id_t)i)) reply("D %s", storage_mount_path((storage_id_t)i) + 1);
        reply("OK");
        return;
    }
    char path[LINE_MAX_LEN];
    if (!vol_path(arg, path, sizeof path)) { reply("ERR bad path: start with /sd or /usb"); return; }
    storage_id_t id;
    if (!vol_hold(path, &id)) { vol_release(id); reply("ERR %s is not mounted", storage_label(id)); return; }
    DIR *d = opendir(path);
    if (!d) {
        reply("ERR cannot open %s", path);
    } else {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            char full[LINE_MAX_LEN + 260];
            snprintf(full, sizeof full, "%s/%s", path, e->d_name);
            struct stat st;
            if (e->d_type == DT_DIR) reply("D %s", e->d_name);
            else if (stat(full, &st) == 0) reply("F %lld %s", (long long)st.st_size, e->d_name);
            else reply("F ? %s", e->d_name);
        }
        closedir(d);
        reply("OK");
    }
    vol_release(id);
}

/* ---- YMODEM, on the console's port ---------------------------------------- */

static size_t yrd(void *ctx, uint8_t *buf, size_t n, int ms)
{
    (void)ctx;
    int r = usb_serial_jtag_read_bytes(buf, n, pdMS_TO_TICKS(ms));
    return r > 0 ? (size_t)r : 0;
}

static void yflush(void *ctx)
{
    uint8_t b[256];
    while (yrd(ctx, b, sizeof b, 50)) { }
}

static void ywr(void *ctx, const uint8_t *buf, size_t n)
{
    (void)ctx;
    usb_serial_jtag_write_bytes(buf, n, portMAX_DELAY);
}

static void yprog(void *ctx, const char *name, uint64_t done, uint64_t total)
{
    (void)ctx;
    xfer_file(name, done, total);
}

/* rb [-f] [-g] <dir>: receive a YMODEM batch into dir. -g asks for
 * YMODEM-G (streaming); -f replaces files that are there. */
static void cmd_rb(char *arg)
{
    bool force = false, stream = false;
    while (arg[0] == '-') {
        for (char *p = arg + 1; *p && *p != ' '; p++) {
            if (*p == 'f') force = true;
            else if (*p == 'g') stream = true;
            else { reply("ERR usage: rb [-f] [-g] <dir>"); return; }
        }
        while (*arg && *arg != ' ') arg++;
        while (*arg == ' ') arg++;
    }
    char dir[LINE_MAX_LEN];
    if (!*arg || !vol_path(arg, dir, sizeof dir)) { reply("ERR bad path: /sd/... or /usb/..."); return; }
    storage_id_t id;
    if (!vol_hold(dir, &id)) {
        vol_release(id);
        reply("ERR %s is not mounted", storage_label(id));
        return;
    }
    char probe[LINE_MAX_LEN + 4];
    snprintf(probe, sizeof probe, "%s/x", dir);
    make_parents(probe);
    struct stat st;
    if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        vol_release(id);
        reply("ERR cannot make %s", dir);
        return;
    }

    reply("READY %s %s, start the YMODEM send", stream ? "ymodem-g" : "ymodem", dir);
    /* Nothing else may reach the port until the transfer is over: a log
     * line is bytes the sender reads as protocol, and the 'C' in "CHG_EN"
     * is a request to start. */
    esp_log_level_t was = esp_log_level_get("*");
    esp_log_level_set("*", ESP_LOG_NONE);
    xfer_begin("Receiving", dir);
    const ymodem_io_t io = { .read = yrd, .flush_in = yflush, .write = ywr, .progress = yprog };
    ymodem_result_t res;
    bool ok = ymodem_receive(&io, dir, force, stream, &res);
    xfer_end();
    vTaskDelay(pdMS_TO_TICKS(200));     /* let a sender finish and stop listening */
    yflush(NULL);
    esp_log_level_set("*", was);
    vol_release(id);
    printf("\r\n");
    if (ok) reply("OK %d files, %llu bytes", res.files, (unsigned long long)res.bytes);
    else reply("ERR %s (%d files completed)", res.msg, res.files);
}

/* sha256 <path>: "OK <hex>", read back from the card or the drive. */
static void cmd_sha256(const char *arg)
{
    char path[LINE_MAX_LEN];
    if (!vol_path(arg, path, sizeof path)) { reply("ERR bad path: /sd/... or /usb/..."); return; }
    storage_id_t id;
    if (!vol_hold(path, &id)) { vol_release(id); reply("ERR %s is not mounted", storage_label(id)); return; }
    FILE *f = fopen(path, "rb");
    if (!f) {
        reply("ERR cannot open %s", path);
    } else {
        static uint8_t buf[16 * 1024];
        sha256_t h;
        sha256_init(&h);
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) sha256_update(&h, buf, n);
        bool err = ferror(f);
        fclose(f);
        if (err) {
            reply("ERR read failed");
        } else {
            uint8_t d[32];
            char hex[65];
            sha256_final(&h, d);
            for (int i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", d[i]);
            reply("OK %s", hex);
        }
    }
    vol_release(id);
}

static void cmd_rm_mkdir(bool mk, const char *arg)
{
    char path[LINE_MAX_LEN];
    if (!vol_path(arg, path, sizeof path) || is_root(path)) { reply("ERR bad path: /sd/... or /usb/..."); return; }
    storage_id_t id;
    if (!vol_hold(path, &id)) {
        reply("ERR %s is not mounted", storage_label(id));
    } else if (mk) {
        char probe[LINE_MAX_LEN + 4];
        snprintf(probe, sizeof probe, "%s/x", path);
        make_parents(probe);
        struct stat st;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) reply("OK");
        else reply("ERR mkdir: %s", strerror(errno));
    } else {
        struct stat st;
        int r = stat(path, &st) != 0 ? -1 : S_ISDIR(st.st_mode) ? rmdir(path) : unlink(path);
        if (r == 0) reply("OK");
        else reply("ERR rm: %s", strerror(errno));
    }
    vol_release(id);
}

static void progress(size_t done, size_t total, void *ctx)
{
    xfer_file(ctx, done, total);
    static size_t last;
    if (done == 0 || done == total || done - last >= 256 * 1024) {
        reply("PROGRESS %u/%u", (unsigned)done, (unsigned)total);
        last = done;
    }
}

static void handle(char *line)
{
    char *arg = line;
    while (*arg && *arg != ' ') arg++;
    if (*arg) *arg++ = '\0';
    while (*arg == ' ') arg++;
    const char *cmd = line;

    if (!strcasecmp(cmd, "version")) {
        reply("%s", duplicitous_title());
        reply("OK");
        return;
    }
    if (!strcasecmp(cmd, "whoami")) { reply("M5Stack Tab5"); reply("OK"); return; }
    if (!strcasecmp(cmd, "help")) {
        reply("version | whoami | help | ls [dir] | df [vol] | rb [-f] [-g] <dir> | sha256 <path>");
        reply("rm <path> | mkdir <path> | install <path> | boot | reboot");
        reply("OK");
        return;
    }
    if (!strcasecmp(cmd, "reboot")) { reply("OK rebooting"); vTaskDelay(pdMS_TO_TICKS(100)); esp_restart(); }
    if (!strcasecmp(cmd, "boot")) {
        if (!install_present(NULL, 0)) { reply("ERR app2 is empty"); return; }
        reply("OK booting app2");
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_err_t e = install_boot();
        reply("ERR %s", esp_err_to_name(e));
        return;
    }

    /* everything below touches a volume */
    work_lock();
    if (!strcasecmp(cmd, "ls")) {
        cmd_ls(arg);
    } else if (!strcasecmp(cmd, "df")) {
        uint64_t total, free_bytes;
        char path[LINE_MAX_LEN];
        if (!vol_path(*arg ? arg : "/sd", path, sizeof path)) reply("ERR bad path: /sd or /usb");
        else if (vol_space(path, &total, &free_bytes) == ESP_OK)
            reply("OK total=%llu free=%llu", (unsigned long long)total, (unsigned long long)free_bytes);
        else reply("ERR %s is not mounted", storage_label(storage_of_path(path)));
    } else if (!strcasecmp(cmd, "rb")) {
        cmd_rb(arg);
    } else if (!strcasecmp(cmd, "sha256")) {
        cmd_sha256(arg);
    } else if (!strcasecmp(cmd, "rm") || !strcasecmp(cmd, "mkdir")) {
        cmd_rm_mkdir(!strcasecmp(cmd, "mkdir"), arg);
    } else if (!strcasecmp(cmd, "install")) {
        char path[LINE_MAX_LEN], err[128];
        if (!vol_path(arg, path, sizeof path)) reply("ERR bad path: /sd/... or /usb/...");
        else {
            const char *base = strrchr(path, '/');
            xfer_begin("Installing", path);
            bool ok = install_from_file(path, progress, (void *)(base ? base + 1 : path), err, sizeof err) == ESP_OK;
            xfer_end();
            if (ok) reply("OK installed");
            else reply("ERR %s", err);
        }
    } else {
        reply("ERR unknown command, type 'help' for command list");
    }
    work_unlock();
}

static void console_task(void *arg)
{
    (void)arg;
    static char line[LINE_MAX_LEN];
    size_t len = 0;
    bool overflow = false;
    for (;;) {
        uint8_t c;
        if (usb_serial_jtag_read_bytes(&c, 1, portMAX_DELAY) != 1) continue;
        if (c == '\r') continue;
        if (c != '\n') {
            if (len < sizeof line - 1) line[len++] = (char)c;
            else overflow = true;
            continue;
        }
        line[len] = '\0';
        while (len && line[len - 1] == ' ') line[--len] = '\0';
        char *start = line;
        while (*start == ' ') start++;
        if (overflow) reply("ERR line too long");
        else if (*start) handle(start);
        len = 0;
        overflow = false;
    }
}

esp_err_t console_start(void)
{
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.rx_buffer_size = 4096;
    cfg.tx_buffer_size = 4096;
    esp_err_t e = usb_serial_jtag_driver_install(&cfg);
    if (e != ESP_OK) return e;
    /* printf and the logs through the driver too, so they do not fight
     * it for the port */
    usb_serial_jtag_vfs_use_driver();
    /* only warnings from here on: a log line in the middle of a transfer
     * is noise a host has to skip */
    esp_log_level_set("*", ESP_LOG_WARN);
    ESP_LOGI(TAG, "ready");
    xTaskCreate(console_task, "console", 8192, NULL, 5, NULL);
    return ESP_OK;
}
