/*
 * vol.h -- the two volumes, as the rest of Avoidant sees them.
 *
 * feckless_storage_handler mounts the microSD card at /sd and a USB
 * drive at /usb, FAT or exFAT, and remounts them as they come and go.
 * Anything that opens a file on one holds it first (vol_hold()), so it
 * is not unmounted underneath the read, and releases after the close.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "storage.h"

/*
 * "/sd/bins/a.bin", "sd/bins/a.bin" or "/usb" -> "/sd/bins/a.bin" (no
 * trailing slash). False for anything not on /sd or /usb, or with a ".."
 * component.
 */
bool vol_path(const char *in, char *out, size_t out_len);

/* Hold the volume path is on and say whether it is mounted. Always
 * paired with vol_release(), mounted or not. */
bool vol_hold(const char *path, storage_id_t *id);
void vol_release(storage_id_t id);

/* Bytes on the volume path is on, and bytes free. */
esp_err_t vol_space(const char *path, uint64_t *total, uint64_t *free_bytes);
