/*
 * ymodem.h -- receive files by YMODEM batch, into a directory.
 *
 * The protocol is Chuck Forsberg's (XMODEM/YMODEM Protocol Reference,
 * 1988), written from that document for this file: block 0 carries the
 * file's name and length, then 128- or 1024-byte blocks with a CRC-16,
 * then EOT; an empty block 0 ends the batch. Two ways to ask for it:
 *
 *   'C'  YMODEM: every block acknowledged, a bad one sent again.
 *   'G'  YMODEM-G: the sender streams, nothing is acknowledged, and any
 *        error cancels the batch. For links that do not lose bytes, which
 *        the Tab5's native USB is; several times faster.
 *
 * A file's name may contain '/', and lands in that subdirectory of the
 * target directory (made as needed); a leading '/' and any ".." are
 * refused. Each file is written as <name>.part and renamed when complete,
 * so a cancelled transfer leaves nothing half-written under the name.
 * An existing file cancels the batch unless `overwrite` is set: the
 * receiver cannot decline one file politely, only stop.
 *
 * Nothing here is ESP-IDF: the port is reached through ymodem_io_t, so
 * the receiver is tested on the host against lrzsz's sz and against the
 * web page's sender (m5tab5/test/ymodem/).
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    /* Up to n bytes, waiting up to ms for the first; 0 on timeout. */
    size_t (*read)(void *ctx, uint8_t *buf, size_t n, int ms);
    /* Discard whatever has arrived and not been read. */
    void (*flush_in)(void *ctx);
    void (*write)(void *ctx, const uint8_t *buf, size_t n);
    /* A file starts (done 0), goes on, and ends (done == total). total 0
     * if the sender did not say. May be NULL. */
    void (*progress)(void *ctx, const char *name, uint64_t done, uint64_t total);
    void *ctx;
} ymodem_io_t;

typedef struct {
    int      files;         /* completed */
    uint64_t bytes;         /* in those files */
    char     msg[160];      /* why it stopped, when it failed */
} ymodem_result_t;

/* Receive a batch into dir (which must exist). True when every file the
 * sender offered arrived complete. */
bool ymodem_receive(const ymodem_io_t *io, const char *dir, bool overwrite,
                    bool streaming, ymodem_result_t *out);
