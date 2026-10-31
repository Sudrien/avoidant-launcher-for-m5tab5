/*
 * settings.h -- what the gear's page changes, kept in NVS (0028).
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "esp_log.h"

/* The console's log level: what reaches the USB-C port, Avoidant's
 * and a running .elf app's alike. WARN until set. A transfer still
 * silences everything while it runs (console.c), whatever this is. */
esp_log_level_t settings_log_level(void);

/* Set it, apply it now, and keep it for the next boot. */
void settings_set_log_level(esp_log_level_t level);
