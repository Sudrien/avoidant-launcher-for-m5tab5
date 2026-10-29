/*
 * elfrun.c -- see elfrun.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "elfrun.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"

#include "vol.h"

#define EI_NIDENT   16
#define ET_DYN      3
#define EM_RISCV    243

/* The front of an ELF32 header: enough to say what it is. */
typedef struct {
    uint8_t  ident[EI_NIDENT];
    uint16_t type;
    uint16_t machine;
} elf_head_t;

static bool check(FILE *f, size_t size, char *why, size_t why_len)
{
#define NO(...) do { snprintf(why, why_len, __VA_ARGS__); return false; } while (0)
    elf_head_t h;
    if (fseek(f, 0, SEEK_SET) != 0 || fread(&h, sizeof h, 1, f) != 1) NO("too short for an ELF");
    if (memcmp(h.ident, "\x7f" "ELF", 4) != 0) NO("not an ELF");
    if (h.ident[4] != 1 || h.ident[5] != 1) NO("not 32-bit little-endian");
    if (h.machine != EM_RISCV) NO("built for machine %u, not RISC-V", h.machine);
    if (h.type != ET_DYN) NO("not a shared object (type %u): not feckless_abi_elf()'s", h.type);
    if (size > ELFRUN_MAX) NO("%u KB is more than %u KB", (unsigned)(size / 1024),
                              (unsigned)(ELFRUN_MAX / 1024));
    return true;
#undef NO
}

bool elfrun_probe(const char *path, char *why, size_t why_len)
{
    struct stat st;
    if (stat(path, &st) != 0) { snprintf(why, why_len, "cannot read it"); return false; }
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(why, why_len, "cannot open it"); return false; }
    bool ok = check(f, (size_t)st.st_size, why, why_len);
    fclose(f);
    return ok;
}

#define FAIL(...) do { snprintf(err, err_len, __VA_ARGS__); ret = ESP_FAIL; goto out; } while (0)

esp_err_t elfrun_load(const char *path, uint8_t **image, size_t *len,
                      install_progress_fn cb, void *ctx, char *err, size_t err_len)
{
    esp_err_t ret = ESP_OK;
    FILE *f = NULL;
    uint8_t *buf = NULL;
    storage_id_t vid;
    work_lock();
    if (!vol_hold(path, &vid)) FAIL("the card or drive went away");

    struct stat st;
    if (stat(path, &st) != 0) FAIL("cannot read it");
    const size_t size = (size_t)st.st_size;
    f = fopen(path, "rb");
    if (!f) FAIL("cannot open it");
    char why[96];
    if (!check(f, size, why, sizeof why)) FAIL("%s", why);

    /* PSRAM: esp_elf copies the sections out of this into PSRAM of its
     * own, and internal RAM is the DMA's. */
    buf = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) FAIL("no room for %u KB", (unsigned)(size / 1024));
    if (fseek(f, 0, SEEK_SET) != 0) FAIL("cannot read it");
    size_t done = 0;
    if (cb) cb(0, size, ctx);
    while (done < size) {
        size_t want = size - done > 32768 ? 32768 : size - done;
        size_t got = fread(buf + done, 1, want, f);
        if (got == 0) FAIL("read failed at %u KB", (unsigned)(done / 1024));
        done += got;
        if (cb) cb(done, size, ctx);
    }
    *image = buf;
    *len = size;
    buf = NULL;

out:
    if (f) fclose(f);
    heap_caps_free(buf);
    vol_release(vid);
    work_unlock();
    return ret;
}
