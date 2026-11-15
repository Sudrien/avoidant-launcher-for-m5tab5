/*
 * boxns.c -- see boxns.h. (u-avoidant-0004)
 *
 * SPDX-License-Identifier: MIT
 */
#include "boxns.h"

#include <string.h>

/* What this firmware keeps in NVS, and what its libraries and ESP-IDF do:
 * install.c's and settings.c's "duplicitous"; feckless_network_handler's
 * "wifistore", "feckless_https" (the device's certificate key) and
 * "feckless_mail"; ESP-IDF's "phy" and "nvs.net80211". A namespace with a
 * '.' never comes from a stem. Prefixes catch the libraries' next ones,
 * at the cost of an app called physics.app.elf getting no NVS. */
static const char *const k_kept[] = { "duplicitous", "avoidant", "wifistore", "wifi", "phy", "misc", "storage" };
static const char *const k_kept_prefix[] = { "feckless", "nvs", "esp", "phy", "wifi" };

bool boxns_for(const char *file, char out[BOXNS_LEN + 1])
{
    out[0] = '\0';
    if (!file) return false;
    size_t n = 0;
    for (const char *p = file; *p && *p != '_' && *p != '.'; p++, n++) {
        if (n == BOXNS_LEN) {
            out[0] = '\0';
            return false;
        }
        const char c = *p;
        if (c >= 'A' && c <= 'Z') {
            out[n] = (char)(c - 'A' + 'a');
        } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            out[n] = c;
        } else {
            out[0] = '\0';
            return false;
        }
    }
    out[n] = '\0';
    bool ok = n > 0;
    for (size_t i = 0; ok && i < sizeof k_kept / sizeof k_kept[0]; i++) {
        if (strcmp(out, k_kept[i]) == 0) ok = false;
    }
    for (size_t i = 0; ok && i < sizeof k_kept_prefix / sizeof k_kept_prefix[0]; i++) {
        if (strncmp(out, k_kept_prefix[i], strlen(k_kept_prefix[i])) == 0) ok = false;
    }
    if (!ok) out[0] = '\0';
    return ok;
}
