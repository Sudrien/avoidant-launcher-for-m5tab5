/*
 * clock.c -- see clock.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "clock.h"

#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "feckless_net.h"
#include "rtc8130.h"
#include "tab5io.h"
#include "wifi.h"
#include "wifistore.h"

static const char *TAG = "clock";

/* 2026-01-01T00:00:00Z. Earlier than this is an RTC that lost its cell
 * (it starts at 2000) or a reply nobody should believe. */
#define CLOCK_FLOOR     1767225600LL

static void log_time(const char *what, int64_t epoch)
{
    time_t t = (time_t)epoch;
    struct tm tm;
    gmtime_r(&t, &tm);
    char s[32];
    strftime(s, sizeof s, "%Y-%m-%d %H:%M:%S", &tm);
    ESP_LOGW(TAG, "%s: %s UTC", what, s);
}

static bool ntp_enabled(void) { return true; }

/* 0053: the radio is Avoidant's to start and join, never the worker's:
 * its background join would race the boot join below and the Wi-Fi
 * page's scans, and nothing here could stop it mid-join before an app. */
static bool wifi_wanted(void) { return false; }
static bool radio_owned(void) { return true; }

/* The system clock has already moved (wifi.c); the RTC follows it. */
static bool ntp_reply(int64_t epoch, int64_t boot_us)
{
    (void)boot_us;
    if (epoch < CLOCK_FLOOR) {
        ESP_LOGW(TAG, "NTP reply %lld is before 2026; RTC left alone", (long long)epoch);
        return false;
    }
    if (rtc8130_write(epoch)) log_time("NTP to RTC", epoch);
    else ESP_LOGE(TAG, "NTP reply not written: no RTC");
    return true;
}

void clock_start(void)
{
    if (rtc8130_init(tab5io_bus())) {
        int64_t epoch;
        if (rtc8130_read(&epoch) && epoch >= CLOCK_FLOOR) {
            struct timeval tv = { .tv_sec = (time_t)epoch };
            settimeofday(&tv, NULL);
            log_time("RTC to system clock", epoch);
        } else {
            ESP_LOGW(TAG, "RTC unset or lost its cell; waiting for NTP");
        }
    } else {
        ESP_LOGE(TAG, "no RTC found");
    }
    /* usb_register_class NULL keeps the USB host's, set by feckless_net_start() */
    static const feckless_net_hooks_t hooks = {
        .ntp_enabled = ntp_enabled,
        .ntp_reply   = ntp_reply,
        .wifi_enabled   = wifi_wanted,      /* 0053 */
        .portal_running = radio_owned,      /* 0053 */
    };
    feckless_net_set_hooks(&hooks);
}

/* ---- 0053: a saved network at the menu, for NTP; down before an app ---- */

#define NTP_WAIT_MS     15000

static SemaphoreHandle_t s_done;        /* given when the task has ended */
static volatile bool s_cancel;
static volatile bool s_running;

static void net_task(void *arg)
{
    (void)arg;
    wifi_seen_t *seen = calloc(32, sizeof *seen);
    wifistore_cred_t *cand = calloc(WIFISTORE_MAX, sizeof *cand);
    if (!seen || !cand) goto out;
    if (wifi_start() != ESP_OK) { ESP_LOGE(TAG, "boot join: the radio did not come up"); goto out; }
    if (s_cancel) goto out;
    const int n = wifi_scan_list_quiet(seen, 32);
    const char *names[32];
    int8_t rssi[32];
    for (int i = 0; i < n; i++) { names[i] = seen[i].ssid; rssi[i] = seen[i].rssi; }
    const int k = n > 0 ? wifistore_rank(names, rssi, n, cand, WIFISTORE_MAX) : 0;
    if (!k) ESP_LOGW(TAG, "boot join: no saved network in range");
    bool joined = false;
    for (int i = 0; i < k && !joined && !s_cancel; i++) {
        joined = wifi_join(cand[i].ssid, cand[i].secret, WIFI_JOIN_TIMEOUT_MS) == ESP_OK;
        ESP_LOGW(TAG, "boot join: %.32s %s", cand[i].ssid, joined ? "joined" : "refused or not answering");
    }
    /* NTP starts on the address; wait for its reply, or a cancel */
    for (int t = 0; joined && !s_cancel && !wifi_ntp_synced() && t < NTP_WAIT_MS; t += 100)
        vTaskDelay(pdMS_TO_TICKS(100));
    if (joined && !wifi_ntp_synced()) ESP_LOGW(TAG, "boot join: no NTP reply yet");
out:
    if (cand) memset(cand, 0, WIFISTORE_MAX * sizeof *cand);
    free(cand);
    free(seen);
    s_running = false;
    xSemaphoreGive(s_done);
    vTaskDelete(NULL);
}

void clock_net_start(void)
{
    if (s_running || wifi_up() || wifistore_count() == 0) return;
    if (!s_done && !(s_done = xSemaphoreCreateBinary())) return;
    xSemaphoreTake(s_done, 0);
    s_cancel = false;
    s_running = true;
    if (xTaskCreate(net_task, "bootnet", 6144, NULL, 4, NULL) != pdPASS) s_running = false;
}

void clock_net_settle(void)
{
    if (!s_running) return;
    s_cancel = true;
    ESP_LOGW(TAG, "waiting for the boot join to finish its step");
    xSemaphoreTake(s_done, portMAX_DELAY);
}

void clock_net_stop(void)
{
    clock_net_settle();
    if (wifi_up()) {
        wifi_stop();
        ESP_LOGW(TAG, "radio down for the app");
    }
}
