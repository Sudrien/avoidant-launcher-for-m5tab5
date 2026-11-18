/*
 * elfsym.h -- does an app's ELF import a name? (u-avoidant-0001)
 *
 * An app built for a box (feckless-libs' feckless_abi_box_elf()) imports
 * feckless_abi_box_v1; one built for the trusted path does not. The ELF is
 * esp_elf's: a 32-bit little-endian shared object whose imports are the
 * undefined entries of .dynsym, which the build's strip keeps. No ESP-IDF in
 * this, so test/elfsym runs it on the host.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* True if the image's .dynsym has an undefined symbol called name. Every
 * offset in the image is checked against len; a damaged ELF answers false. */
bool elfsym_imports(const uint8_t *img, size_t len, const char *name);

/* u-avoidant-0008: the same, read through rd -- n bytes at off into dst,
 * true if all of them were read -- from something len bytes long. What
 * elfrun_probe() uses on a file, so the list can say boxed or trusted
 * without loading the app. name is at most 64 characters. */
typedef bool (*elfsym_read_fn)(void *ctx, size_t off, void *dst, size_t n);
bool elfsym_imports_from(elfsym_read_fn rd, void *ctx, size_t len, const char *name);

#define ELFSYM_BOX_MARK "feckless_abi_box_v1"
