/*
 * settings.c -- see settings.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "settings.h"

#include "nvs.h"

#define NVS_NS      "duplicitous"   /* install.c's namespace */
#define KEY_LOG     "loglevel"

esp_log_level_t settings_log_level(void)
{
    uint8_t v = ESP_LOG_WARN;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, KEY_LOG, &v);
        nvs_close(h);
    }
    return v <= ESP_LOG_VERBOSE ? (esp_log_level_t)v : ESP_LOG_WARN;
}

void settings_set_log_level(esp_log_level_t level)
{
    esp_log_level_set("*", level);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, KEY_LOG, (uint8_t)level);
        nvs_commit(h);
        nvs_close(h);
    }
}
