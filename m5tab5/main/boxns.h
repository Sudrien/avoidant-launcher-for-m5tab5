/*
 * boxns.h -- the NVS namespace a boxed app gets (u-avoidant-0004)
 *
 * feckless-libs' box answers NVS only in namespaces the launcher names
 * (u-feckless-0053). Avoidant names one per app, from its file name: the
 * name up to its first '_' or '.', lower case letters and digits only, at
 * most 15 characters -- defeatist_boxed.app.elf keeps its settings in
 * "defeatist", where Defeatist's own firmware keeps them. A name this
 * firmware or its libraries keep for themselves is refused, so a file
 * renamed wifistore.app.elf cannot read the saved networks. No ESP-IDF in
 * this, so test/boxns runs it on the host.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>

#define BOXNS_LEN 15

/* The namespace for the app in file (a name, not a path) into out; false,
 * and out empty, if it has none: a stem too long, empty, with any other
 * character, or one kept for this firmware. */
bool boxns_for(const char *file, char out[BOXNS_LEN + 1]);
