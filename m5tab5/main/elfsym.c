/*
 * elfsym.c -- see elfsym.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "elfsym.h"

#include <string.h>

#define SHT_DYNSYM  11
#define SHDR_SIZE   40
#define SYM_SIZE    16

static uint32_t u32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t u16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }

/* [off, off+n) inside [0, len), with no wrap. */
static bool in(size_t len, uint32_t off, uint32_t n)
{
    return off <= len && n <= len - off;
}

bool elfsym_imports(const uint8_t *img, size_t len, const char *name)
{
    if (!img || !name || len < 52 || memcmp(img, "\x7f" "ELF", 4) != 0 || img[4] != 1 || img[5] != 1) {
        return false;
    }
    const uint32_t shoff = u32(img + 32);
    const uint16_t shentsize = u16(img + 46), shnum = u16(img + 48);
    if (shentsize != SHDR_SIZE || !in(len, shoff, (uint32_t)shnum * SHDR_SIZE)) {
        return false;
    }
    const size_t nlen = strlen(name);
    for (uint16_t i = 0; i < shnum; i++) {
        const uint8_t *sh = img + shoff + (size_t)i * SHDR_SIZE;
        if (u32(sh + 4) != SHT_DYNSYM) {
            continue;
        }
        const uint32_t symoff = u32(sh + 16), symsize = u32(sh + 20), link = u32(sh + 24);
        if (link >= shnum || !in(len, symoff, symsize)) {
            return false;
        }
        const uint8_t *st = img + shoff + (size_t)link * SHDR_SIZE;
        const uint32_t stroff = u32(st + 16), strsize = u32(st + 20);
        if (!in(len, stroff, strsize)) {
            return false;
        }
        for (uint32_t s = 0; s + SYM_SIZE <= symsize; s += SYM_SIZE) {
            const uint8_t *sym = img + symoff + s;
            const uint32_t nm = u32(sym);
            if (u16(sym + 14) != 0 || nm >= strsize) {          /* defined, or a bad name */
                continue;
            }
            /* The name and its NUL inside the string table. */
            if (nlen < strsize - nm && memcmp(img + stroff + nm, name, nlen + 1) == 0) {
                return true;
            }
        }
    }
    return false;
}
