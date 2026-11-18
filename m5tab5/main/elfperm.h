/*
 * elfperm.h -- what an app's ELF would use, read from the ELF itself
 * (u-avoidant-0009).
 *
 * An app declares nothing: there is no manifest in feckless_abi's ELFs.
 * What can be known before it runs is what it imports -- a boxed app
 * reaches the board only through host calls, each imported by name, and
 * the load refuses one the box does not answer -- and the paths and file
 * types written into its read-only data. So:
 *
 *   - groups: each import sorted into what it lets the app do, by the
 *     tables in feckless-libs' feckless_box_calls (box_storage.c,
 *     box_nvs.c, box_audio.c, box_camera.c, box_drivers.c, box_calls.c) as
 *     of u-feckless-0066. A call those gain later is "other" here until
 *     this list learns it.
 *   - paths: NUL-terminated strings in allocated, non-executable data that
 *     begin "/sd", "/usb", "0:/" or "1:/" -- folders and files it names,
 *     printf patterns and all ("/sd/tap-%lu.wav").
 *   - file types: strings that are a dot and 2 to 5 letters or digits
 *     (".flac"), lower-cased.
 *
 * Paths an app builds at run time from what it is given (a folder the
 * listener chose) are not in its data and not here. The import is what is
 * certain; the paths are what it says, not all it can reach.
 *
 * No ESP-IDF in this, so test/elfperm runs it on the host.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "elfsym.h"

enum {
    ELFPERM_READ    = 1u << 0,     /* opens and reads files, lists folders */
    ELFPERM_WRITE   = 1u << 1,     /* writes files, makes folders */
    ELFPERM_DELETE  = 1u << 2,     /* deletes or renames files and folders */
    ELFPERM_NVS_R   = 1u << 3,     /* reads its settings */
    ELFPERM_NVS_W   = 1u << 4,     /* saves or erases its settings */
    ELFPERM_SOUND   = 1u << 5,     /* plays sound */
    ELFPERM_MIC     = 1u << 6,     /* records from a microphone */
    ELFPERM_USB     = 1u << 7,     /* USB host: powers the port, USB audio */
    ELFPERM_CAMERA  = 1u << 8,
    ELFPERM_PINS    = 1u << 9,     /* UART, I2C, GPIO */
    ELFPERM_CLOCK   = 1u << 10,    /* sets the clock */
    ELFPERM_RESTART = 1u << 11,    /* restarts the board */
    ELFPERM_BATTERY = 1u << 12,
    ELFPERM_NET     = 1u << 13,    /* a trusted app's Wi-Fi, sockets, HTTP; the box has none */
};
#define ELFPERM_GROUPS 14

#define ELFPERM_PATHS     16
#define ELFPERM_PATH_LEN  64       /* longer paths are cut, and end "..." */
#define ELFPERM_EXTS      32
#define ELFPERM_EXT_LEN   8

typedef struct {
    uint32_t groups;               /* ELFPERM_* bits */
    uint32_t imports;              /* every import */
    uint32_t other;                /* imports in no group: the panel, heap, maths, threads */
    char     paths[ELFPERM_PATHS][ELFPERM_PATH_LEN];
    uint32_t npaths, paths_more;   /* kept, and found past the room */
    char     exts[ELFPERM_EXTS][ELFPERM_EXT_LEN];
    uint32_t nexts, exts_more;
} elfperm_t;

/* The group of one import name: an ELFPERM_* bit, or 0. */
uint32_t elfperm_classify(const char *name);

/* The words for one bit, for a screen: "Writes files". NULL for 0 or more
 * than one bit. */
const char *elfperm_label(uint32_t bit);

/* Read it all through rd (elfsym's reader) into *out. False, with what was
 * found so far in *out, for a damaged ELF. */
bool elfperm_scan(elfsym_read_fn rd, void *ctx, size_t len, elfperm_t *out);
