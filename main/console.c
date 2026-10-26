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
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "install.h"
#include "sd.h"

static const char *TAG = "console";

#define LINE_MAX_LEN  1024
#define PUT_CHUNK     2048
#define PUT_TIMEOUT   pdMS_TO_TICKS(5000)

static void reply(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    fflush(stdout);
}

/* "bins/a.bin" or "/bins/a.bin" -> "/sd/bins/a.bin". False for a path
 * that would leave the card. */
static bool card_path(const char *in, char *out, size_t out_len)
{
    while (*in == '/') in++;
    for (const char *p = in; (p = strstr(p, "..")) != NULL; p += 2) {
        bool start = p == in || p[-1] == '/';
        bool end = p[2] == '\0' || p[2] == '/';
        if (start && end) return false;
    }
    int n = snprintf(out, out_len, *in ? SD_MOUNT "/%s" : SD_MOUNT, in);
    if (n < 0 || (size_t)n >= out_len) return false;
    /* no trailing slash; FatFs dislikes it */
    size_t l = strlen(out);
    while (l > strlen(SD_MOUNT) && out[l - 1] == '/') out[--l] = '\0';
    return true;
}

/* mkdir -p for the directories above path. */
static void make_parents(const char *path)
{
    char tmp[LINE_MAX_LEN];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + strlen(SD_MOUNT) + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        mkdir(tmp, 0775);
        *p = '/';
    }
}

/* Exactly n bytes from the port, or fewer if PUT_TIMEOUT passes with
 * nothing arriving. */
static size_t read_exact(uint8_t *buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        int r = usb_serial_jtag_read_bytes(buf + got, n - got, PUT_TIMEOUT);
        if (r <= 0) break;
        got += (size_t)r;
    }
    return got;
}

static void cmd_ls(const char *arg)
{
    char path[LINE_MAX_LEN];
    if (!card_path(arg, path, sizeof path)) { reply("ERR bad path"); return; }
    DIR *d = opendir(path);
    if (!d) { reply("ERR cannot open %s", path); return; }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        char full[LINE_MAX_LEN + 260];
        snprintf(full, sizeof full, "%s/%s", path, e->d_name);
        struct stat st;
        if (e->d_type == DT_DIR) reply("D %s", e->d_name);
        else if (stat(full, &st) == 0) reply("F %ld %s", (long)st.st_size, e->d_name);
        else reply("F ? %s", e->d_name);
    }
    closedir(d);
    reply("OK");
}

static void cmd_put(const char *arg)
{
    char *end;
    unsigned long size = strtoul(arg, &end, 0);
    while (*end == ' ') end++;
    char path[LINE_MAX_LEN], part[LINE_MAX_LEN + 8];
    if (end == arg || !*end) { reply("ERR usage: put <size> <path>"); return; }
    if (!card_path(end, path, sizeof path) || !strcmp(path, SD_MOUNT)) { reply("ERR bad path"); return; }
    snprintf(part, sizeof part, "%s.part", path);

    uint64_t total, free_bytes;
    if (sd_space(&total, &free_bytes) != ESP_OK) { reply("ERR no card"); return; }
    if (size > free_bytes) { reply("ERR %lu bytes do not fit, %llu free", size, (unsigned long long)free_bytes); return; }

    make_parents(path);
    FILE *f = fopen(part, "wb");
    if (!f) { reply("ERR cannot create %s: %s", part, strerror(errno)); return; }

    static uint8_t buf[PUT_CHUNK];
    const char *fail = NULL;
    unsigned long done = 0;
    reply("READY %lu", size);
    while (done < size) {
        size_t want = size - done < PUT_CHUNK ? size - done : PUT_CHUNK;
        size_t got = read_exact(buf, want);
        if (got == 0) { fail = "stream read timeout"; break; }
        if (fwrite(buf, 1, got, f) != got) { fail = "card write failed"; break; }
        done += got;
        /* per chunk the host sent -- it never has more than one in flight */
        if (got == want) reply("ACK %lu/%lu", done, size);
        else { fail = "stream read timeout"; break; }
    }
    if (fclose(f) != 0 && !fail) fail = "card write failed";
    if (fail) { unlink(part); reply("ERR %s", fail); return; }
    unlink(path);                       /* FatFs rename will not replace */
    if (rename(part, path) != 0) { unlink(part); reply("ERR rename: %s", strerror(errno)); return; }
    reply("OK %lu", size);
}

static void progress(size_t done, size_t total, void *ctx)
{
    (void)ctx;
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
        reply("Duplicitous " DUPLICITOUS_VERSION);
        reply("OK");
        return;
    }
    if (!strcasecmp(cmd, "whoami")) { reply("M5Stack Tab5"); reply("OK"); return; }
    if (!strcasecmp(cmd, "help")) {
        reply("version | whoami | help | ls [dir] | df | put <size> <path> | rm <path>");
        reply("mkdir <path> | install <path> | boot | reboot");
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

    /* everything below touches the card */
    work_lock();
    if (!sd_mounted() && sd_mount() != ESP_OK) {
        reply("ERR no card");
    } else if (!strcasecmp(cmd, "ls")) {
        cmd_ls(arg);
    } else if (!strcasecmp(cmd, "df")) {
        uint64_t total, free_bytes;
        if (sd_space(&total, &free_bytes) == ESP_OK)
            reply("OK total=%llu free=%llu", (unsigned long long)total, (unsigned long long)free_bytes);
        else reply("ERR no card");
    } else if (!strcasecmp(cmd, "put")) {
        cmd_put(arg);
    } else if (!strcasecmp(cmd, "rm") || !strcasecmp(cmd, "mkdir")) {
        char path[LINE_MAX_LEN];
        if (!*arg || !card_path(arg, path, sizeof path) || !strcmp(path, SD_MOUNT)) reply("ERR bad path");
        else if (!strcasecmp(cmd, "mkdir")) {
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
    } else if (!strcasecmp(cmd, "install")) {
        char path[LINE_MAX_LEN], err[128];
        if (!*arg || !card_path(arg, path, sizeof path)) reply("ERR bad path");
        else if (install_from_file(path, progress, NULL, err, sizeof err) == ESP_OK) reply("OK installed");
        else reply("ERR %s", err);
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
