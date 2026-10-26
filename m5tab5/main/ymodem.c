/*
 * ymodem.c -- see ymodem.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "ymodem.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SOH 0x01    /* 128-byte block */
#define STX 0x02    /* 1024-byte block */
#define EOT 0x04
#define ACK 0x06
#define NAK 0x15
#define CAN 0x18

#define BLOCK_MS     10000  /* a block's bytes, once one has started */
#define START_MS     1000   /* between 'C'/'G' while waiting for a sender */
#define START_TRIES  60     /* about a minute to start the sender */
#define RETRIES      10     /* bad blocks in a row, in 'C' mode */

typedef struct {
    const ymodem_io_t *io;
    bool streaming;
    uint8_t blk[1024 + 5];
} rx_t;

static void put1(rx_t *r, uint8_t c) { r->io->write(r->io->ctx, &c, 1); }

static void cancel(rx_t *r)
{
    static const uint8_t c[] = { CAN, CAN, CAN, CAN, CAN, 8, 8, 8, 8, 8 };
    r->io->write(r->io->ctx, c, sizeof c);
}

static bool read_exact(rx_t *r, uint8_t *p, size_t n, int ms)
{
    while (n) {
        size_t got = r->io->read(r->io->ctx, p, n, ms);
        if (!got) return false;
        p += got;
        n -= got;
    }
    return true;
}

static uint16_t crc16(const uint8_t *p, size_t n)
{
    uint16_t crc = 0;
    while (n--) {
        crc ^= (uint16_t)*p++ << 8;
        for (int i = 0; i < 8; i++) crc = crc & 0x8000 ? (uint16_t)(crc << 1) ^ 0x1021 : (uint16_t)(crc << 1);
    }
    return crc;
}

typedef enum { B_DATA, B_EOT, B_CAN, B_TIMEOUT, B_BAD } bres_t;

/* One block, or what came instead. On B_DATA, *seq and *len are set and
 * the data is at r->blk + 3. */
static bres_t get_block(rx_t *r, int first_ms, uint8_t *seq, size_t *len)
{
    uint8_t c;
    if (!r->io->read(r->io->ctx, &c, 1, first_ms)) return B_TIMEOUT;
    if (c == EOT) return B_EOT;
    if (c == CAN) {
        /* two in a row is a cancel; one alone is line noise */
        if (r->io->read(r->io->ctx, &c, 1, 1000) && c == CAN) return B_CAN;
        return B_BAD;
    }
    if (c != SOH && c != STX) return B_BAD;
    size_t n = c == STX ? 1024 : 128;
    if (!read_exact(r, r->blk, n + 4, BLOCK_MS)) return B_TIMEOUT;
    if ((uint8_t)(r->blk[0] ^ r->blk[1]) != 0xFF) return B_BAD;
    uint16_t want = (uint16_t)(r->blk[n + 2] << 8 | r->blk[n + 3]);
    if (crc16(r->blk + 2, n) != want) return B_BAD;
    memmove(r->blk + 3, r->blk + 2, n);     /* data at +3, seq at +0 */
    *seq = r->blk[0];
    *len = n;
    return B_DATA;
}

/* A name from block 0 made safe to put under dir. */
static bool safe_name(const char *in, char *out, size_t out_len)
{
    if (!*in || *in == '/' || strlen(in) >= out_len) return false;
    for (const char *p = in; *p; p++) if ((unsigned char)*p < 0x20 || *p == '\\' || *p == ':') return false;
    for (const char *p = in; (p = strstr(p, "..")) != NULL; p += 2) {
        bool start = p == in || p[-1] == '/';
        bool end = p[2] == '\0' || p[2] == '/';
        if (start && end) return false;
    }
    strcpy(out, in);
    return true;
}

static void make_parents(const char *dir, const char *path)
{
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + strlen(dir) + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        mkdir(tmp, 0775);
        *p = '/';
    }
}

#define FAIL(...) do { snprintf(out->msg, sizeof out->msg, __VA_ARGS__); goto fail; } while (0)

bool ymodem_receive(const ymodem_io_t *io, const char *dir, bool overwrite,
                    bool streaming, ymodem_result_t *out)
{
    static rx_t rx;
    rx_t *r = &rx;
    r->io = io;
    r->streaming = streaming;
    memset(out, 0, sizeof *out);
    const uint8_t ask = streaming ? 'G' : 'C';

    FILE *f = NULL;
    char path[512] = "", part[520] = "";
    bool first_file = true;

    for (;;) {
        /* ---- block 0: a name and a length, or the end of the batch ---- */
        uint8_t seq;
        size_t len;
        bres_t b = B_TIMEOUT;
        int tries = first_file ? START_TRIES : RETRIES;
        for (int t = 0; t < tries; t++) {
            put1(r, ask);
            b = get_block(r, START_MS, &seq, &len);
            if (b == B_DATA && seq == 0) break;
            if (b == B_CAN) FAIL("cancelled by the sender");
            if (b == B_DATA || b == B_BAD) io->flush_in(io->ctx);
        }
        if (b != B_DATA || seq != 0) FAIL(first_file ? "no sender started" : "sender stopped between files");
        first_file = false;

        const char *name0 = (const char *)r->blk + 3;
        if (!name0[0]) {            /* empty block 0: the batch is over */
            put1(r, ACK);
            return true;
        }
        char name[256];
        if (!memchr(name0, 0, len) || !safe_name(name0, name, sizeof name)) {
            cancel(r);
            FAIL("refused file name");
        }
        const char *meta = name0 + strlen(name0) + 1;
        uint64_t total = 0;
        if (meta < name0 + len && *meta) total = strtoull(meta, NULL, 10);

        snprintf(path, sizeof path, "%s/%s", dir, name);
        snprintf(part, sizeof part, "%s.part", path);
        struct stat st;
        if (!overwrite && stat(path, &st) == 0) {
            cancel(r);
            FAIL("%.100s exists (rb -f to replace)", name);
        }
        make_parents(dir, path);
        f = fopen(part, "wb");
        if (!f) {
            cancel(r);
            FAIL("cannot create %.100s: %s", part, strerror(errno));
        }
        if (io->progress) io->progress(io->ctx, name, 0, total);

        /* 'C': ACK, then ask again for the data. 'G': just ask again. */
        if (!streaming) put1(r, ACK);
        put1(r, ask);

        /* ---- the data ---- */
        uint8_t expect = 1;
        uint64_t done = 0;
        int bad = 0;
        for (;;) {
            b = get_block(r, BLOCK_MS, &seq, &len);
            if (b == B_EOT) break;
            if (b == B_CAN) FAIL("cancelled by the sender");
            if (b == B_DATA && seq == expect) {
                size_t keep = len;
                if (total && done + keep > total) keep = (size_t)(total - done);
                if (keep && fwrite(r->blk + 3, 1, keep, f) != keep) {
                    cancel(r);
                    FAIL("write failed: %s", strerror(errno));
                }
                done += keep;
                expect++;
                bad = 0;
                if (!streaming) put1(r, ACK);
                if (io->progress) io->progress(io->ctx, name, done, total);
                continue;
            }
            if (b == B_DATA && seq == (uint8_t)(expect - 1) && !streaming) {
                put1(r, ACK);       /* our ACK was lost; they sent it again */
                continue;
            }
            /* a bad block, a timeout, or one out of order */
            if (streaming || b == B_DATA || ++bad > RETRIES) {
                cancel(r);
                FAIL(b == B_TIMEOUT ? "timed out at %llu bytes"
                     : b == B_DATA ? "block out of order at %llu bytes"
                     : "bad block at %llu bytes", (unsigned long long)done);
            }
            io->flush_in(io->ctx);
            put1(r, NAK);
        }

        /* EOT. In 'C' mode the reference sender sends it twice: NAK the
         * first. Either way ACK what ends the file. */
        if (!streaming) {
            put1(r, NAK);
            uint8_t c;
            if (!io->read(io->ctx, &c, 1, 2000) || c != EOT) { /* one EOT only: accept it */ }
        }
        put1(r, ACK);

        if (fclose(f) != 0) { f = NULL; FAIL("write failed closing %.100s", name); }
        f = NULL;
        if (total && done != total) FAIL("%.100s: %llu of %llu bytes", name,
                                         (unsigned long long)done, (unsigned long long)total);
        unlink(path);                       /* FatFs rename will not replace */
        if (rename(part, path) != 0) FAIL("rename %.100s: %s", name, strerror(errno));
        part[0] = '\0';
        out->files++;
        out->bytes += done;
        if (io->progress) io->progress(io->ctx, name, done, done);
    }

fail:
    if (f) fclose(f);
    if (part[0]) unlink(part);
    return false;
}
