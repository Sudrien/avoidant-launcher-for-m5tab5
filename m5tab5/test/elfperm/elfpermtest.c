/*
 * main/elfperm.c on the host (u-avoidant-0009): the import rules, then an
 * ELF built in memory -- .dynstr, .dynsym, a .rodata of paths, types and
 * look-alikes, and a .text whose path must not be read as data -- and
 * every damage to it answered false without reading outside. Any ELF named
 * on the command line gets a report, so a real app can be looked at:
 *
 *   make -C m5tab5/test/elfperm
 *   ./elfpermtest defeatist_player.app.elf
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "elfperm.h"

static int failed;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); failed++; } } while (0)

typedef struct { const uint8_t *b; size_t len; size_t hi; } mem_t;

/* Reads, and remembers the furthest byte asked for. */
static bool mem_read(void *ctx, size_t off, void *dst, size_t n)
{
    mem_t *m = ctx;
    if (off > m->len || n > m->len - off) return false;
    memcpy(dst, m->b + off, n);
    if (off + n > m->hi) m->hi = off + n;
    return true;
}

static bool scan(const uint8_t *b, size_t len, elfperm_t *p)
{
    mem_t m = { b, len, 0 };
    const bool ok = elfperm_scan(mem_read, &m, len, p);
    if (m.hi > len) { printf("FAIL read past the end\n"); failed++; }
    return ok;
}

static bool has_path(const elfperm_t *p, const char *s)
{
    for (uint32_t i = 0; i < p->npaths; i++) if (!strcmp(p->paths[i], s)) return true;
    return false;
}

static bool has_ext(const elfperm_t *p, const char *s)
{
    for (uint32_t i = 0; i < p->nexts; i++) if (!strcmp(p->exts[i], s)) return true;
    return false;
}

static uint8_t img[8192];
static size_t img_len;
#define SHOFF 7000

static void put32(size_t at, uint32_t v) { memcpy(img + at, &v, 4); }
static void put16(size_t at, uint16_t v) { memcpy(img + at, &v, 2); }

static void shdr(int i, uint32_t type, uint32_t flags, uint32_t off, uint32_t size, uint32_t link)
{
    const size_t at = SHOFF + (size_t)i * 40;
    put32(at + 4, type); put32(at + 8, flags); put32(at + 16, off); put32(at + 20, size);
    put32(at + 24, link);
}

static const char *const imports[] = {
    "fopen", "fread", "fwrite", "unlink", "nvsh_get_u8", "nvsh_set_str",
    "audio_out_write", "audio_out_capture_read", "camera_start", "gpio_set_level",
    "esp_restart", "gfx_draw_text", "malloc", "feckless_abi_box_v1",
};
#define NIMP (sizeof imports / sizeof imports[0])

/* Sections: 0 null, 1 .dynstr, 2 .dynsym, 3 .rodata, 4 .text, 5 .comment
 * (not allocated: its path is not the app's). */
static void build(void)
{
    memset(img, 0, sizeof img);
    memcpy(img, "\x7f" "ELF", 4);
    img[4] = 1; img[5] = 1; img[6] = 1;
    put16(16, 3); put16(18, 243);

    /* .dynstr at 64, .dynsym at 1024: the imports undefined, one defined */
    uint32_t so = 64, at = 1, sy = 1024;
    img[so] = 0;
    for (size_t i = 0; i < NIMP; i++) {
        const size_t n = strlen(imports[i]) + 1;
        memcpy(img + so + at, imports[i], n);
        put32(sy + (i + 1) * 16, at);
        at += (uint32_t)n;
    }
    memcpy(img + so + at, "app_main", 9);
    put32(sy + (NIMP + 1) * 16, at); put16(sy + (NIMP + 1) * 16 + 14, 7);   /* defined */
    at += 9;
    shdr(1, 3, 2, so, at, 0);
    shdr(2, 11, 2, sy, (NIMP + 2) * 16, 1);

    /* .rodata at 2048 */
    static const char ro[] =
        "\0/sd/tap-%lu.wav\0/sd\0/usb/Podcasts\0" "0:/music\0" "/sd/tap-%lu.wav\0"
        ".flac\0.MP3\0.m3u8\0.c\0.toolong\0.12\0%.1f\0/sdcard\0usb/\0"
        "x/sd/a\0\x01/sd/b\0" "/sd/ok\0";
    memcpy(img + 2048, ro, sizeof ro);
    shdr(3, 1, 2, 2048, sizeof ro, 0);
    /* .text at 3072: a path in code is not data */
    memcpy(img + 3072, "/sd/in-code\0.text1\0", 19);
    shdr(4, 1, 2 | 4, 3072, 19, 0);
    memcpy(img + 4096, "/sd/in-comment\0", 15);
    shdr(5, 1, 0, 4096, 15, 0);

    put32(32, SHOFF); put16(46, 40); put16(48, 6);
    img_len = SHOFF + 6 * 40;
}

static void report(const char *name, const elfperm_t *p)
{
    printf("%s: %u imports, %u in no group\n", name, (unsigned)p->imports, (unsigned)p->other);
    for (int b = 0; b < ELFPERM_GROUPS; b++)
        if (p->groups >> b & 1) printf("  %s\n", elfperm_label(1u << b));
    for (uint32_t i = 0; i < p->npaths; i++) printf("  path %s\n", p->paths[i]);
    if (p->paths_more) printf("  and %u more paths\n", (unsigned)p->paths_more);
    if (p->nexts) {
        printf("  types");
        for (uint32_t i = 0; i < p->nexts; i++) printf(" %s", p->exts[i]);
        printf(p->exts_more ? " and %u more\n" : "\n", (unsigned)p->exts_more);
    }
}

int main(int argc, char **argv)
{
    /* the rules */
    CHECK(elfperm_classify("fopen") == ELFPERM_READ);
    CHECK(elfperm_classify("fopenx") == 0);                 /* whole names are whole */
    CHECK(elfperm_classify("fwrite") == ELFPERM_WRITE);
    CHECK(elfperm_classify("rename") == ELFPERM_DELETE);
    CHECK(elfperm_classify("nvsh_get_blob") == ELFPERM_NVS_R);
    CHECK(elfperm_classify("nvsh_erase_ns") == ELFPERM_NVS_W);
    CHECK(elfperm_classify("audio_out_capture_begin") == ELFPERM_MIC);  /* before audio_out_ */
    CHECK(elfperm_classify("audio_out_set_volume") == ELFPERM_SOUND);
    CHECK(elfperm_classify("uac_mic_open_bits") == ELFPERM_MIC);
    CHECK(elfperm_classify("uac_write") == ELFPERM_USB);
    CHECK(elfperm_classify("storage_usb_power") == ELFPERM_USB);
    CHECK(elfperm_classify("storage_usb_info") == 0);
    CHECK(elfperm_classify("_uart_set_pin6") == ELFPERM_PINS);
    CHECK(elfperm_classify("i2c_master_transmit") == ELFPERM_PINS);
    CHECK(elfperm_classify("rtc8130_read") == 0);
    CHECK(elfperm_classify("rtc8130_write_forward") == ELFPERM_CLOCK);
    CHECK(elfperm_classify("settimeofday") == ELFPERM_CLOCK);
    CHECK(elfperm_classify("esp_wifi_start") == ELFPERM_NET);
    CHECK(elfperm_classify("feckless_fetch_start") == ELFPERM_NET);
    CHECK(elfperm_classify("gfx_draw_text") == 0);
    CHECK(elfperm_classify("") == 0 && elfperm_classify(NULL) == 0);
    for (int b = 0; b < ELFPERM_GROUPS; b++) CHECK(elfperm_label(1u << b) != NULL);
    CHECK(elfperm_label(0) == NULL && elfperm_label(3) == NULL);

    /* the built ELF */
    static elfperm_t p;
    build();
    CHECK(scan(img, img_len, &p));
    CHECK(p.imports == NIMP);
    CHECK(p.other == 3);                                    /* gfx_draw_text, malloc, the mark */
    CHECK(p.groups == (ELFPERM_READ | ELFPERM_WRITE | ELFPERM_DELETE | ELFPERM_NVS_R |
                       ELFPERM_NVS_W | ELFPERM_SOUND | ELFPERM_MIC | ELFPERM_CAMERA |
                       ELFPERM_PINS | ELFPERM_RESTART));
    CHECK(has_path(&p, "/sd/tap-%lu.wav") && has_path(&p, "/sd") &&
          has_path(&p, "/usb/Podcasts") && has_path(&p, "0:/music") && has_path(&p, "/sd/ok"));
    CHECK(p.npaths == 5);                                   /* the repeat kept once */
    CHECK(!has_path(&p, "/sdcard") && !has_path(&p, "/sd/in-code") &&
          !has_path(&p, "/sd/in-comment") && !has_path(&p, "/sd/b"));
    CHECK(has_ext(&p, ".flac") && has_ext(&p, ".mp3") && has_ext(&p, ".m3u8"));
    CHECK(p.nexts == 3);                                    /* not .c, .toolong, .12, %.1f, .text1 */

    /* room: 40 paths and 50 types, the rest counted */
    {
        static uint8_t big[8192];
        memcpy(big, img, sizeof big);
        size_t o = 5000;
        for (int i = 0; i < 40; i++) o += (size_t)sprintf((char *)big + o, "/sd/p%02d", i) + 1;
        for (int i = 0; i < 50; i++) o += (size_t)sprintf((char *)big + o, ".t%02d", i) + 1;
        /* .rodata now 5000..o; it must stay below SHOFF */
        CHECK(o < SHOFF);
        memcpy(img, big, sizeof big);
        shdr(3, 1, 2, 5000, (uint32_t)(o - 5000), 0);
        CHECK(scan(img, img_len, &p));
        CHECK(p.npaths == ELFPERM_PATHS && p.paths_more == 40 - ELFPERM_PATHS);
        CHECK(p.nexts == ELFPERM_EXTS && p.exts_more == 50 - ELFPERM_EXTS);
    }
    /* a path too long is cut and marked; a run past 255 is not a string */
    build();
    {
        char longp[120];
        memset(longp, 'a', sizeof longp);
        memcpy(longp, "/sd/", 4);
        longp[100] = 0;
        memcpy(img + 2048 + 200, "\0", 1);
        memcpy(img + 2048 + 201, longp, 101);
        memset(img + 2048 + 400, 'b', 300);
        memcpy(img + 2048 + 400, "/sd/", 4);
        img[2048 + 700] = 0;
        shdr(3, 1, 2, 2048, 701, 0);
        CHECK(scan(img, img_len, &p));
        char want[ELFPERM_PATH_LEN];
        memcpy(want, longp, ELFPERM_PATH_LEN - 4);
        memcpy(want + ELFPERM_PATH_LEN - 4, "...", 4);
        CHECK(has_path(&p, want));
        for (uint32_t i = 0; i < p.npaths; i++) CHECK(strncmp(p.paths[i], "/sd/bbb", 7) != 0);
    }

    /* damage: false, nothing read outside */
    build();
    CHECK(!scan(img, 51, &p));
    CHECK(!scan(img, img_len - 1, &p));
    shdr(3, 1, 2, 2048, 0xfffffff0u, 0); CHECK(!scan(img, img_len, &p)); build();
    shdr(3, 1, 2, 0xfffffff0u, 16, 0); CHECK(!scan(img, img_len, &p)); build();
    shdr(2, 11, 2, 1024, 0xfffffff0u, 1); CHECK(!scan(img, img_len, &p)); build();
    shdr(1, 3, 2, 64, 0x7fffffffu, 0); CHECK(!scan(img, img_len, &p)); build();
    /* a .dynstr cut inside a name: that name passed over, the rest seen */
    {
        const uint32_t cut = 1 + 6 + 6 + 7 + 3;              /* inside "unlink" */
        shdr(1, 3, 2, 64, cut, 0);
        CHECK(scan(img, img_len, &p));
        CHECK(!(p.groups & ELFPERM_DELETE) && (p.groups & ELFPERM_WRITE));
        build();
    }

    for (int i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        if (!f) { printf("%s: cannot open\n", argv[i]); failed++; continue; }
        fseek(f, 0, SEEK_END);
        const long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        uint8_t *b = malloc((size_t)n);
        if (fread(b, 1, (size_t)n, f) != (size_t)n) failed++;
        fclose(f);
        if (scan(b, (size_t)n, &p)) report(argv[i], &p);
        else { printf("%s: not an ELF this reads\n", argv[i]); }
        free(b);
    }
    printf(failed ? "%d FAILED\n" : "elfperm: all passed\n", failed);
    return failed != 0;
}
