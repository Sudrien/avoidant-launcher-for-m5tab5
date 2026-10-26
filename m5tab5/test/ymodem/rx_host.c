/*
 * rx_host.c -- main/ymodem.c on the host, on stdin/stdout: the receiver
 * the Tab5 runs, against a real sender. See run.sh.
 *
 *   rx_host [-f] [-g] <dir>
 *
 * SPDX-License-Identifier: MIT
 */
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "ymodem.h"

static size_t rd(void *ctx, uint8_t *buf, size_t n, int ms)
{
    (void)ctx;
    struct pollfd p = { .fd = 0, .events = POLLIN };
    if (poll(&p, 1, ms) <= 0) return 0;
    ssize_t r = read(0, buf, n);
    return r > 0 ? (size_t)r : 0;
}

static void fl(void *ctx)
{
    uint8_t b[4096];
    while (rd(ctx, b, sizeof b, 50)) { }
}

static void wr(void *ctx, const uint8_t *buf, size_t n)
{
    (void)ctx;
    while (n) {
        ssize_t w = write(1, buf, n);
        if (w <= 0) return;
        buf += w;
        n -= (size_t)w;
    }
}

int main(int argc, char **argv)
{
    bool force = false, g = false;
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "-f")) force = true;
        if (!strcmp(argv[i], "-g")) g = true;
    }
    if (i >= argc) { fprintf(stderr, "usage: rx_host [-f] [-g] <dir>\n"); return 2; }
    const ymodem_io_t io = { .read = rd, .flush_in = fl, .write = wr };
    ymodem_result_t res;
    bool ok = ymodem_receive(&io, argv[i], force, g, &res);
    fprintf(stderr, "rx_host: %s, %d files, %llu bytes%s%s\n", ok ? "OK" : "ERR", res.files,
            (unsigned long long)res.bytes, res.msg[0] ? ": " : "", res.msg);
    return ok ? 0 : 1;
}
