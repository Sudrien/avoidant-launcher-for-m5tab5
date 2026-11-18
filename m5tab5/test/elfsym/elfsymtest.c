/*
 * main/elfsym.c on the host: an ELF built in memory the shape esp_elf's are
 * (ELF32, little-endian, .dynsym and .dynstr), every damage to it answered
 * false, and any ELF named on the command line reported, so the real thing
 * can be checked:
 *
 *   make -C m5tab5/test/elfsym
 *   ./elfsymtest some.app.elf ...
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "elfsym.h"

static int failed;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); failed++; } } while (0)

static uint8_t img[1024];
static size_t img_len;
static uint32_t symoff, stroff;

/* The list's way: the bytes in a file, read at offsets through stdio. */
static bool file_read(void *ctx, size_t off, void *dst, size_t n)
{
    FILE *f = ctx;
    return fseek(f, (long)off, SEEK_SET) == 0 && fread(dst, 1, n, f) == n;
}

static bool via_file(const uint8_t *b, size_t len, const char *name)
{
    FILE *f = tmpfile();
    if (!f) { printf("FAIL no tmpfile\n"); failed++; return false; }
    if (len && b && fwrite(b, 1, len, f) != len) failed++;
    const bool r = elfsym_imports_from(file_read, f, len, name);
    fclose(f);
    return r;
}

/* Memory and file, which must agree; the answer is memory's. */
static bool ask(const uint8_t *b, size_t len, const char *name)
{
    const bool m = elfsym_imports(b, len, name);
    if (b && via_file(b, len, name) != m) {
        printf("FAIL memory and file disagree on %s (len %zu)\n", name ? name : "(null)", len);
        failed++;
    }
    return m;
}

static void put32(size_t at, uint32_t v) { memcpy(img + at, &v, 4); }
static void put16(size_t at, uint16_t v) { memcpy(img + at, &v, 2); }

/* An ELF with sections: 0 null, 1 .dynstr, 2 .dynsym. Symbols: null,
 * "gfx_draw_text" undefined, the mark (undefined, or defined), "app_main"
 * defined. */
static void build(int mark_undefined)
{
    memset(img, 0, sizeof img);
    memcpy(img, "\x7f" "ELF", 4);
    img[4] = 1; img[5] = 1; img[6] = 1;
    put16(16, 3);           /* ET_DYN */
    put16(18, 243);         /* RISC-V */
    const char str[] = "\0gfx_draw_text\0" ELFSYM_BOX_MARK "\0app_main\0";
    stroff = 64;
    memcpy(img + stroff, str, sizeof str);
    const uint32_t n_gfx = 1, n_mark = 15, n_main = 15 + sizeof ELFSYM_BOX_MARK;
    symoff = 256;
    put32(symoff + 16, n_gfx);
    put32(symoff + 32, n_mark); put16(symoff + 32 + 14, mark_undefined ? 0 : 7);
    put32(symoff + 48, n_main); put16(symoff + 48 + 14, 7);
    const uint32_t shoff = 512;
    put32(32, shoff); put16(46, 40); put16(48, 3);
    put32(shoff + 40 + 4, 3);  put32(shoff + 40 + 16, stroff); put32(shoff + 40 + 20, sizeof str);
    put32(shoff + 80 + 4, 11); put32(shoff + 80 + 16, symoff); put32(shoff + 80 + 20, 64); put32(shoff + 80 + 24, 1);
    img_len = shoff + 3 * 40;
}

int main(int argc, char **argv)
{
    build(1);
    CHECK(ask(img, img_len, ELFSYM_BOX_MARK));
    CHECK(ask(img, img_len, "gfx_draw_text"));
    CHECK(!ask(img, img_len, "app_main"));                  /* defined, not imported */
    CHECK(!ask(img, img_len, "gfx_draw"));                  /* a prefix is not the name */
    CHECK(!ask(img, img_len, "gfx_draw_text_more"));
    build(0);
    CHECK(!ask(img, img_len, ELFSYM_BOX_MARK));             /* defined there: a trusted app */

    /* damage: each answers false and reads nothing outside */
    build(1);
    CHECK(!ask(img, 51, ELFSYM_BOX_MARK));
    CHECK(!ask(img, img_len - 1, ELFSYM_BOX_MARK));         /* section headers cut */
    img[4] = 2;  CHECK(!ask(img, img_len, ELFSYM_BOX_MARK)); build(1);   /* ELF64 */
    img[0] = 0;  CHECK(!ask(img, img_len, ELFSYM_BOX_MARK)); build(1);
    put32(32, 0xfffffff0u); CHECK(!ask(img, img_len, ELFSYM_BOX_MARK)); build(1);
    put32(512 + 80 + 16, 1000); CHECK(!ask(img, img_len, ELFSYM_BOX_MARK)); build(1);   /* .dynsym past the end */
    put32(512 + 80 + 20, 0xfffffff0u); CHECK(!ask(img, img_len, ELFSYM_BOX_MARK)); build(1);
    put32(512 + 80 + 24, 9); CHECK(!ask(img, img_len, ELFSYM_BOX_MARK)); build(1);      /* link past shnum */
    put32(512 + 40 + 20, 0x7fffffffu); CHECK(!ask(img, img_len, ELFSYM_BOX_MARK)); build(1);
    /* a string table cut inside the mark's name: no NUL inside it, so no match */
    put32(512 + 40 + 20, 15 + 10); CHECK(!ask(img, img_len, ELFSYM_BOX_MARK)); build(1);
    put32(symoff + 32, 0xffffff00u); CHECK(!ask(img, img_len, ELFSYM_BOX_MARK)); build(1);
    CHECK(!ask(NULL, 10, "x") && !ask(img, img_len, NULL));

    /* u-avoidant-0008: a .dynsym of 70 symbols, the mark at 33 -- past the
     * first batch of 32 -- then at 69, the last; and a name longer than 64. */
    {
        static uint8_t big[4096];
        memset(big, 0, sizeof big);
        memcpy(big, img, 64);
        const char str2[] = "\0x\0" ELFSYM_BOX_MARK "\0";
        const uint32_t so = 64, sy = 256, nsym = 70, sho = sy + nsym * 16;
        memcpy(big + so, str2, sizeof str2);
        for (int at = 33; at <= 69; at += 36) {
            memset(big + sy, 0, nsym * 16);
            for (uint32_t k = 1; k < nsym; k++) {
                uint32_t nm = (k == (uint32_t)at) ? 3 : 1;
                memcpy(big + sy + k * 16, &nm, 4);
                uint16_t shndx = (k == (uint32_t)at) ? 0 : 7;
                memcpy(big + sy + k * 16 + 14, &shndx, 2);
            }
            uint32_t v; uint16_t w;
            v = sho; memcpy(big + 32, &v, 4); w = 40; memcpy(big + 46, &w, 2); w = 3; memcpy(big + 48, &w, 2);
            memset(big + sho, 0, 120);
            v = 3;  memcpy(big + sho + 44, &v, 4); v = so; memcpy(big + sho + 56, &v, 4);
            v = sizeof str2; memcpy(big + sho + 60, &v, 4);
            v = 11; memcpy(big + sho + 84, &v, 4); v = sy; memcpy(big + sho + 96, &v, 4);
            v = nsym * 16; memcpy(big + sho + 100, &v, 4); v = 1; memcpy(big + sho + 104, &v, 4);
            const size_t bl = sho + 120;
            CHECK(ask(big, bl, ELFSYM_BOX_MARK));
            CHECK(!ask(big, bl, "feckless_abi_box_v"));
        }
        char longname[80];
        memset(longname, 'a', 65); longname[65] = 0;
        CHECK(!ask(big, sizeof big, longname));
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
        printf("%s: %s\n", argv[i], ask(b, (size_t)n, ELFSYM_BOX_MARK) ? "boxed" : "trusted");
        free(b);
    }
    printf(failed ? "%d FAILED\n" : "elfsym: all passed\n", failed);
    return failed != 0;
}
