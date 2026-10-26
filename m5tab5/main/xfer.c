/*
 * xfer.c -- see xfer.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "xfer.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static xfer_t s_x;
static uint32_t s_gen;

void xfer_begin(const char *what, const char *where)
{
    xfer_t x = { .active = true };
    snprintf(x.what, sizeof x.what, "%s", what);
    snprintf(x.where, sizeof x.where, "%s", where);
    taskENTER_CRITICAL(&s_mux);
    s_x = x;
    s_gen++;
    taskEXIT_CRITICAL(&s_mux);
}

void xfer_file(const char *name, uint64_t done, uint64_t total)
{
    char n[sizeof s_x.name];
    snprintf(n, sizeof n, "%s", name);
    taskENTER_CRITICAL(&s_mux);
    memcpy(s_x.name, n, sizeof n);
    s_x.done = done;
    s_x.total = total;
    s_gen++;
    taskEXIT_CRITICAL(&s_mux);
}

void xfer_end(void)
{
    taskENTER_CRITICAL(&s_mux);
    s_x.active = false;
    s_gen++;
    taskEXIT_CRITICAL(&s_mux);
}

uint32_t xfer_get(xfer_t *out)
{
    taskENTER_CRITICAL(&s_mux);
    *out = s_x;
    uint32_t g = s_gen;
    taskEXIT_CRITICAL(&s_mux);
    return g;
}
