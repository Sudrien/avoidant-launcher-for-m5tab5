/*
 * sha256.h -- SHA-256, for checking a file on the card against the
 * sender's.
 *
 * Vendored rather than taken from Mbed TLS: Mbed TLS 4.0 (ESP-IDF 6.1)
 * dropped the legacy mbedtls/sha256.h, and reaching SHA-256 through PSA
 * differs again from the 3.x path on IDF 5.x -- the same reason
 * defeatist-music-player-for-m5tab5 vendors its AES. One small
 * implementation builds on every IDF.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t h[8];
    uint64_t len;       /* bytes so far */
    uint8_t  buf[64];
    size_t   used;
} sha256_t;

void sha256_init(sha256_t *s);
void sha256_update(sha256_t *s, const void *data, size_t n);
void sha256_final(sha256_t *s, uint8_t out[32]);
