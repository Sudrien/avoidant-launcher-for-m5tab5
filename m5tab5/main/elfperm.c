/*
 * elfperm.c -- see elfperm.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "elfperm.h"

#include <string.h>

#define SHT_PROGBITS   1
#define SHF_ALLOC      2u
#define SHF_EXECINSTR  4u
#define SHDR_SIZE      40
#define CHUNK          1024
#define RUN_MAX        255        /* a printable run longer than this is not a path */

static uint32_t u32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t u16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }

/* A name ending '_' is a prefix; any other, the whole name. First match
 * wins, so the narrow entries come before the wide ones. */
static const struct { const char *name; uint32_t group; } s_rules[] = {
    /* box_storage.c */
    { "fopen", ELFPERM_READ }, { "fread", ELFPERM_READ }, { "fgets", ELFPERM_READ },
    { "fseek", ELFPERM_READ }, { "ftell", ELFPERM_READ }, { "opendir", ELFPERM_READ },
    { "readdir", ELFPERM_READ }, { "rewinddir", ELFPERM_READ }, { "stat", ELFPERM_READ },
    { "f_open", ELFPERM_READ }, { "f_read", ELFPERM_READ }, { "f_lseek", ELFPERM_READ },
    { "f_opendir", ELFPERM_READ }, { "f_readdir", ELFPERM_READ },
    { "storage_io_open", ELFPERM_READ }, { "storage_io_fread", ELFPERM_READ },
    { "storage_io_read_at", ELFPERM_READ },
    { "fwrite", ELFPERM_WRITE }, { "fflush", ELFPERM_WRITE }, { "fsync", ELFPERM_WRITE },
    { "mkdir", ELFPERM_WRITE }, { "utime", ELFPERM_WRITE }, { "f_chmod", ELFPERM_WRITE },
    { "f_write", ELFPERM_WRITE }, { "storage_mark_hidden", ELFPERM_WRITE },
    { "remove", ELFPERM_DELETE }, { "unlink", ELFPERM_DELETE }, { "rename", ELFPERM_DELETE },
    { "rmdir", ELFPERM_DELETE }, { "f_unlink", ELFPERM_DELETE }, { "f_rename", ELFPERM_DELETE },
    { "storage_usb_power", ELFPERM_USB },
    /* box_nvs.c */
    { "nvsh_get_", ELFPERM_NVS_R },
    { "nvsh_set_", ELFPERM_NVS_W }, { "nvsh_erase_", ELFPERM_NVS_W },
    /* box_audio.c: the microphone before the rest of audio_out_ */
    { "audio_out_capture_", ELFPERM_MIC }, { "uac_mic_", ELFPERM_MIC },
    { "audio_out_", ELFPERM_SOUND }, { "uac_", ELFPERM_USB }, { "usbhost_", ELFPERM_USB },
    { "battery_", ELFPERM_BATTERY },
    { "rtc8130_write", ELFPERM_CLOCK }, { "rtc8130_write_forward", ELFPERM_CLOCK },
    /* box_camera.c, box_drivers.c */
    { "camera_", ELFPERM_CAMERA },
    { "uart_", ELFPERM_PINS }, { "_uart_", ELFPERM_PINS }, { "i2c_", ELFPERM_PINS },
    { "gpio_", ELFPERM_PINS },
    /* box_calls.c */
    { "settimeofday", ELFPERM_CLOCK }, { "esp_restart", ELFPERM_RESTART },
    /* a trusted app's network; nothing in the box answers these */
    { "esp_wifi_", ELFPERM_NET }, { "esp_netif_", ELFPERM_NET }, { "esp_http_client_", ELFPERM_NET },
    { "esp_websocket_", ELFPERM_NET }, { "lwip_", ELFPERM_NET }, { "socket", ELFPERM_NET },
    { "connect", ELFPERM_NET }, { "getaddrinfo", ELFPERM_NET }, { "wifi_", ELFPERM_NET },
    { "net_", ELFPERM_NET }, { "esp_tls_", ELFPERM_NET }, { "mbedtls_ssl_", ELFPERM_NET },
    /* feckless_network_handler */
    { "wifistore_", ELFPERM_NET }, { "netlink_", ELFPERM_NET }, { "ethernet_", ELFPERM_NET },
    { "feckless_fetch_", ELFPERM_NET }, { "feckless_https_", ELFPERM_NET },
    { "feckless_net_", ELFPERM_NET },
};

uint32_t elfperm_classify(const char *name)
{
    if (!name) return 0;
    for (size_t i = 0; i < sizeof s_rules / sizeof s_rules[0]; i++) {
        const char *r = s_rules[i].name;
        const size_t n = strlen(r);
        if (r[n - 1] == '_' ? strncmp(name, r, n) == 0 : strcmp(name, r) == 0) {
            return s_rules[i].group;
        }
    }
    return 0;
}

const char *elfperm_label(uint32_t bit)
{
    switch (bit) {
    case ELFPERM_READ:    return "Reads files and folders";
    case ELFPERM_WRITE:   return "Writes files, makes folders";
    case ELFPERM_DELETE:  return "Deletes or renames files";
    case ELFPERM_NVS_R:   return "Reads settings";
    case ELFPERM_NVS_W:   return "Saves settings";
    case ELFPERM_SOUND:   return "Plays sound";
    case ELFPERM_MIC:     return "Records from a microphone";
    case ELFPERM_USB:     return "USB host: port power, audio";
    case ELFPERM_CAMERA:  return "Uses the camera";
    case ELFPERM_PINS:    return "Pins: UART, I2C, GPIO";
    case ELFPERM_CLOCK:   return "Sets the clock";
    case ELFPERM_RESTART: return "Restarts the board";
    case ELFPERM_BATTERY: return "Reads the battery";
    case ELFPERM_NET:     return "Network: Wi-Fi, sockets";
    default:              return NULL;
    }
}

static void on_import(const char *name, void *arg)
{
    elfperm_t *p = arg;
    p->imports++;
    const uint32_t g = elfperm_classify(name);
    if (g) p->groups |= g;
    else p->other++;
}

static bool is_path(const char *s)
{
    return !strcmp(s, "/sd") || !strcmp(s, "/usb") || !strncmp(s, "/sd/", 4) ||
           !strncmp(s, "/usb/", 5) || !strncmp(s, "0:/", 3) || !strncmp(s, "1:/", 3);
}

static bool is_ext(const char *s, size_t n)
{
    if (n < 3 || n > 6 || s[0] != '.') return false;
    bool letter = false;
    for (size_t i = 1; i < n; i++) {
        const char c = s[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) letter = true;
        else if (c < '0' || c > '9') return false;
    }
    return letter;
}

static void keep_path(elfperm_t *p, const char *s, size_t n)
{
    char cut[ELFPERM_PATH_LEN];
    if (n >= sizeof cut) {
        memcpy(cut, s, sizeof cut - 4);
        memcpy(cut + sizeof cut - 4, "...", 4);
    } else {
        memcpy(cut, s, n + 1);
    }
    for (uint32_t i = 0; i < p->npaths; i++) {
        if (!strcmp(p->paths[i], cut)) return;
    }
    if (p->npaths < ELFPERM_PATHS) memcpy(p->paths[p->npaths++], cut, sizeof cut);
    else p->paths_more++;
}

static void keep_ext(elfperm_t *p, const char *s, size_t n)
{
    char low[ELFPERM_EXT_LEN];
    for (size_t i = 0; i <= n; i++) {
        const char c = s[i];
        low[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    for (uint32_t i = 0; i < p->nexts; i++) {
        if (!strcmp(p->exts[i], low)) return;
    }
    if (p->nexts < ELFPERM_EXTS) memcpy(p->exts[p->nexts++], low, n + 1);
    else p->exts_more++;
}

/* The strings of one section: runs of printable bytes that begin the
 * section or follow a NUL, and end at a NUL. One that follows another byte
 * is the tail of something that is not a string -- a table, an image. */
static bool scan_strings(elfsym_read_fn rd, void *ctx, uint32_t off, uint32_t size, elfperm_t *p)
{
    uint8_t buf[CHUNK];
    char run[RUN_MAX + 1];
    size_t rn = 0;
    bool over = false;          /* this run is not a string: too long, or a bad start */
    for (uint32_t at = 0; at < size;) {
        const size_t n = size - at < CHUNK ? size - at : CHUNK;
        if (!rd(ctx, (size_t)off + at, buf, n)) return false;
        for (size_t i = 0; i < n; i++) {
            const uint8_t c = buf[i];
            if (c >= 0x20 && c < 0x7f) {
                if (rn < RUN_MAX) run[rn++] = (char)c;
                else over = true;
                continue;
            }
            if (c == 0 && rn && !over) {
                run[rn] = 0;
                if (is_path(run)) keep_path(p, run, rn);
                else if (is_ext(run, rn)) keep_ext(p, run, rn);
            }
            rn = 0;
            over = c != 0;      /* what follows this byte starts clean only after a NUL */
        }
        at += (uint32_t)n;
    }
    return true;
}

bool elfperm_scan(elfsym_read_fn rd, void *ctx, size_t len, elfperm_t *out)
{
    memset(out, 0, sizeof *out);
    if (!elfsym_each_import(rd, ctx, len, on_import, out)) return false;

    /* elfsym_each_import() has checked the header and the section table. */
    uint8_t h[52];
    if (!rd(ctx, 0, h, sizeof h)) return false;
    const uint32_t shoff = u32(h + 32);
    const uint16_t shnum = u16(h + 48);
    for (uint16_t i = 0; i < shnum; i++) {
        uint8_t sh[SHDR_SIZE];
        if (!rd(ctx, shoff + (size_t)i * SHDR_SIZE, sh, sizeof sh)) return false;
        const uint32_t type = u32(sh + 4), flags = u32(sh + 8);
        const uint32_t off = u32(sh + 16), size = u32(sh + 20);
        if (type != SHT_PROGBITS || !(flags & SHF_ALLOC) || (flags & SHF_EXECINSTR)) continue;
        if (off > len || size > len - off) return false;
        if (!scan_strings(rd, ctx, off, size, out)) return false;
    }
    return true;
}
