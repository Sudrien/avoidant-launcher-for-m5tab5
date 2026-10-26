/*
 * xfer.h -- what the console is doing to the card, for the screen.
 *
 * The console task sets it as a transfer or an install goes; ui_task
 * reads a copy and shows it, so whoever is at the Tab5 can see that a
 * computer is writing, what, and how far it has got. That is the whole
 * of the console's "permission": anyone with the cable could flash over
 * Duplicitous anyway (README).
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool     active;
    char     what[24];      /* "Receiving", "Installing" */
    char     where[64];     /* the directory, or the file */
    char     name[128];     /* the file now */
    uint64_t done, total;   /* total 0: not known */
} xfer_t;

void xfer_begin(const char *what, const char *where);
void xfer_file(const char *name, uint64_t done, uint64_t total);
void xfer_end(void);

/* A copy of the state, and a counter that moves whenever it changes. */
uint32_t xfer_get(xfer_t *out);
