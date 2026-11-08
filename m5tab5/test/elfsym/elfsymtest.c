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
    CHECK(elfsym_imports(img, img_len, ELFSYM_BOX_MARK));
    CHECK(elfsym_imports(img, img_len, "gfx_draw_text"));
    CHECK(!elfsym_imports(img, img_len, "app_main"));                  /* defined, not imported */
    CHECK(!elfsym_imports(img, img_len, "gfx_draw"));                  /* a prefix is not the name */
    CHECK(!elfsym_imports(img, img_len, "gfx_draw_text_more"));
    build(0);
    CHECK(!elfsym_imports(img, img_len, ELFSYM_BOX_MARK));             /* defined there: a trusted app */

    /* damage: each answers false and reads nothing outside */
    build(1);
    CHECK(!elfsym_imports(img, 51, ELFSYM_BOX_MARK));
    CHECK(!elfsym_imports(img, img_len - 1, ELFSYM_BOX_MARK));         /* section headers cut */
    img[4] = 2;  CHECK(!elfsym_imports(img, img_len, ELFSYM_BOX_MARK)); build(1);   /* ELF64 */
    img[0] = 0;  CHECK(!elfsym_imports(img, img_len, ELFSYM_BOX_MARK)); build(1);
    put32(32, 0xfffffff0u); CHECK(!elfsym_imports(img, img_len, ELFSYM_BOX_MARK)); build(1);
    put32(512 + 80 + 16, 1000); CHECK(!elfsym_imports(img, img_len, ELFSYM_BOX_MARK)); build(1);   /* .dynsym past the end */
    put32(512 + 80 + 20, 0xfffffff0u); CHECK(!elfsym_imports(img, img_len, ELFSYM_BOX_MARK)); build(1);
    put32(512 + 80 + 24, 9); CHECK(!elfsym_imports(img, img_len, ELFSYM_BOX_MARK)); build(1);      /* link past shnum */
    put32(512 + 40 + 20, 0x7fffffffu); CHECK(!elfsym_imports(img, img_len, ELFSYM_BOX_MARK)); build(1);
    /* a string table cut inside the mark's name: no NUL inside it, so no match */
    put32(512 + 40 + 20, 15 + 10); CHECK(!elfsym_imports(img, img_len, ELFSYM_BOX_MARK)); build(1);
    put32(symoff + 32, 0xffffff00u); CHECK(!elfsym_imports(img, img_len, ELFSYM_BOX_MARK)); build(1);
    CHECK(!elfsym_imports(NULL, 10, "x") && !elfsym_imports(img, img_len, NULL));

    for (int i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        if (!f) { printf("%s: cannot open\n", argv[i]); failed++; continue; }
        fseek(f, 0, SEEK_END);
        const long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        uint8_t *b = malloc((size_t)n);
        if (fread(b, 1, (size_t)n, f) != (size_t)n) failed++;
        fclose(f);
        printf("%s: %s\n", argv[i], elfsym_imports(b, (size_t)n, ELFSYM_BOX_MARK) ? "boxed" : "trusted");
        free(b);
    }
    printf(failed ? "%d FAILED\n" : "elfsym: all passed\n", failed);
    return failed != 0;
}
