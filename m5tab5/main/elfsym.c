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
#define SYM_BATCH   32          /* symbols read at a time: 512 bytes of stack */
#define NAME_MAX_   ELFSYM_NAME_MAX   /* the longest name this is asked for */

static uint32_t u32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t u16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }

/* [off, off+n) inside [0, len), with no wrap. */
static bool in(size_t len, uint32_t off, uint32_t n)
{
    return off <= len && n <= len - off;
}

/* u-avoidant-0008: one walk, over whatever can be read at an offset -- an
 * image in memory (run_elf(), after the load) or a file on the card (the
 * list, before it), so the two cannot disagree on what a box app is. It
 * reads the header, one section header at a time, the symbols in batches
 * and, for an undefined symbol whose name could be the one asked for, that
 * name's bytes alone: a few KB for an app of several MB, never the image. */
bool elfsym_imports_from(elfsym_read_fn rd, void *ctx, size_t len, const char *name)
{
    uint8_t h[52];
    if (!rd || !name || len < sizeof h || !rd(ctx, 0, h, sizeof h) ||
        memcmp(h, "\x7f" "ELF", 4) != 0 || h[4] != 1 || h[5] != 1) {
        return false;
    }
    const size_t nlen = strlen(name);
    if (nlen == 0 || nlen > NAME_MAX_) {
        return false;
    }
    const uint32_t shoff = u32(h + 32);
    const uint16_t shentsize = u16(h + 46), shnum = u16(h + 48);
    if (shentsize != SHDR_SIZE || !in(len, shoff, (uint32_t)shnum * SHDR_SIZE)) {
        return false;
    }
    for (uint16_t i = 0; i < shnum; i++) {
        uint8_t sh[SHDR_SIZE];
        if (!rd(ctx, shoff + (size_t)i * SHDR_SIZE, sh, sizeof sh)) {
            return false;
        }
        if (u32(sh + 4) != SHT_DYNSYM) {
            continue;
        }
        const uint32_t symoff = u32(sh + 16), symsize = u32(sh + 20), link = u32(sh + 24);
        if (link >= shnum || !in(len, symoff, symsize)) {
            return false;
        }
        uint8_t st[SHDR_SIZE];
        if (!rd(ctx, shoff + (size_t)link * SHDR_SIZE, st, sizeof st)) {
            return false;
        }
        const uint32_t stroff = u32(st + 16), strsize = u32(st + 20);
        if (!in(len, stroff, strsize)) {
            return false;
        }
        uint8_t batch[SYM_BATCH * SYM_SIZE];
        const uint32_t nsym = symsize / SYM_SIZE;
        for (uint32_t s0 = 0; s0 < nsym; s0 += SYM_BATCH) {
            const uint32_t n = nsym - s0 < SYM_BATCH ? nsym - s0 : SYM_BATCH;
            if (!rd(ctx, symoff + (size_t)s0 * SYM_SIZE, batch, (size_t)n * SYM_SIZE)) {
                return false;
            }
            for (uint32_t k = 0; k < n; k++) {
                const uint8_t *sym = batch + k * SYM_SIZE;
                const uint32_t nm = u32(sym);
                if (u16(sym + 14) != 0 || nm >= strsize) {      /* defined, or a bad name */
                    continue;
                }
                /* The name and its NUL inside the string table. */
                if (nlen >= strsize - nm) {
                    continue;
                }
                char got[NAME_MAX_ + 1];
                if (!rd(ctx, stroff + nm, got, nlen + 1)) {
                    return false;
                }
                if (memcmp(got, name, nlen + 1) == 0) {
                    return true;
                }
            }
        }
    }
    return false;
}

/* u-avoidant-0009: the same walk, every import to fn. */
bool elfsym_each_import(elfsym_read_fn rd, void *ctx, size_t len, elfsym_name_fn fn, void *arg)
{
    uint8_t h[52];
    if (!rd || !fn || len < sizeof h || !rd(ctx, 0, h, sizeof h) ||
        memcmp(h, "\x7f" "ELF", 4) != 0 || h[4] != 1 || h[5] != 1) {
        return false;
    }
    const uint32_t shoff = u32(h + 32);
    const uint16_t shentsize = u16(h + 46), shnum = u16(h + 48);
    if (shentsize != SHDR_SIZE || !in(len, shoff, (uint32_t)shnum * SHDR_SIZE)) {
        return false;
    }
    for (uint16_t i = 0; i < shnum; i++) {
        uint8_t sh[SHDR_SIZE];
        if (!rd(ctx, shoff + (size_t)i * SHDR_SIZE, sh, sizeof sh)) {
            return false;
        }
        if (u32(sh + 4) != SHT_DYNSYM) {
            continue;
        }
        const uint32_t symoff = u32(sh + 16), symsize = u32(sh + 20), link = u32(sh + 24);
        if (link >= shnum || !in(len, symoff, symsize)) {
            return false;
        }
        uint8_t st[SHDR_SIZE];
        if (!rd(ctx, shoff + (size_t)link * SHDR_SIZE, st, sizeof st)) {
            return false;
        }
        const uint32_t stroff = u32(st + 16), strsize = u32(st + 20);
        if (!in(len, stroff, strsize)) {
            return false;
        }
        uint8_t batch[SYM_BATCH * SYM_SIZE];
        const uint32_t nsym = symsize / SYM_SIZE;
        for (uint32_t s0 = 0; s0 < nsym; s0 += SYM_BATCH) {
            const uint32_t n = nsym - s0 < SYM_BATCH ? nsym - s0 : SYM_BATCH;
            if (!rd(ctx, symoff + (size_t)s0 * SYM_SIZE, batch, (size_t)n * SYM_SIZE)) {
                return false;
            }
            for (uint32_t k = 0; k < n; k++) {
                const uint8_t *sym = batch + k * SYM_SIZE;
                const uint32_t nm = u32(sym);
                if (u16(sym + 14) != 0 || nm == 0 || nm >= strsize) {
                    continue;
                }
                char got[NAME_MAX_ + 1];
                size_t want = strsize - nm < sizeof got ? strsize - nm : sizeof got;
                if (!rd(ctx, stroff + nm, got, want)) {
                    return false;
                }
                if (memchr(got, 0, want) && got[0]) {
                    fn(got, arg);
                }
            }
        }
    }
    return true;
}

typedef struct { const uint8_t *img; size_t len; } mem_t;

static bool mem_read(void *ctx, size_t off, void *dst, size_t n)
{
    const mem_t *m = ctx;
    if (off > m->len || n > m->len - off) {
        return false;
    }
    memcpy(dst, m->img + off, n);
    return true;
}

bool elfsym_imports(const uint8_t *img, size_t len, const char *name)
{
    if (!img) {
        return false;
    }
    mem_t m = { img, len };
    return elfsym_imports_from(mem_read, &m, len, name);
}
