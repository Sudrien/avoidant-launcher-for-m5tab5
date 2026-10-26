/*
 * sd.h -- the microSD slot, mounted at /sd.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define SD_MOUNT "/sd"

/* Power the slot's IO and mount. Safe to call again after a failure (or
 * to remount after the card was swapped): it unmounts first. */
esp_err_t sd_mount(void);
bool sd_mounted(void);

/* Bytes on the volume and bytes free. */
esp_err_t sd_space(uint64_t *total, uint64_t *free_bytes);
