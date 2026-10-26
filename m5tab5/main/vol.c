/*
 * vol.c -- see vol.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "vol.h"

#include <stdio.h>
#include <string.h>

#include "esp_vfs_fat.h"

bool vol_path(const char *in, char *out, size_t out_len)
{
    while (*in == '/') in++;
    for (const char *p = in; (p = strstr(p, "..")) != NULL; p += 2) {
        bool start = p == in || p[-1] == '/';
        bool end = p[2] == '\0' || p[2] == '/';
        if (start && end) return false;
    }
    int n = snprintf(out, out_len, "/%s", in);
    if (n < 0 || (size_t)n >= out_len) return false;
    size_t l = strlen(out);
    while (l > 1 && out[l - 1] == '/') out[--l] = '\0';
    return storage_of_path(out) != STORAGE_COUNT;
}

bool vol_hold(const char *path, storage_id_t *id)
{
    *id = storage_of_path(path);
    storage_hold_brief(*id);    /* STORAGE_COUNT: takes nothing */
    return *id != STORAGE_COUNT && storage_present(*id);
}

void vol_release(storage_id_t id)
{
    if (id != STORAGE_COUNT) storage_release_brief(id);
}

esp_err_t vol_space(const char *path, uint64_t *total, uint64_t *free_bytes)
{
    storage_id_t id;
    esp_err_t e = ESP_ERR_INVALID_STATE;
    if (vol_hold(path, &id)) e = esp_vfs_fat_info(storage_mount_path(id), total, free_bytes);
    vol_release(id);
    return e;
}
