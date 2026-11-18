/*
 * elfrun.h -- run an app's .elf from the card, on this board, without a
 * restart: feckless-abi-for-m5tab5's esp_elf loader.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "elfperm.h"
#include "install.h"

/* Whether path is an ELF elfrun_load() would take: 32-bit RISC-V, a
 * shared object (what feckless_abi_elf() makes), not larger than
 * ELFRUN_MAX. False with a reason otherwise. The caller holds the volume.
 *
 * u-avoidant-0008: and, if boxed is not NULL, whether it is built for a box
 * (it imports ELFSYM_BOX_MARK, as feckless_abi_box_elf() makes it) or for
 * the trusted path -- read from its section headers and .dynsym, a few KB,
 * not the whole file. run_elf() asks again of the loaded image, and that
 * answer is the one that decides how it runs. */
bool elfrun_probe(const char *path, char *why, size_t why_len, bool *boxed);

#define ELFRUN_MAX  (4 * 1024 * 1024)

/* Read path into PSRAM. *image is the caller's, and must stay allocated
 * while the app runs. Takes work_lock() and holds the volume itself. */
esp_err_t elfrun_load(const char *path, uint8_t **image, size_t *len,
                      install_progress_fn cb, void *ctx, char *err, size_t err_len);

/* u-avoidant-0009: what the ELF at path would use (elfperm.h), read from the
 * file -- its imports, and the strings of its data sections, so up to its
 * whole size in the worst case and a few hundred KB for most. Takes
 * work_lock() and holds the volume itself, as elfrun_load() does. */
esp_err_t elfrun_perms(const char *path, elfperm_t *out, char *err, size_t err_len);
