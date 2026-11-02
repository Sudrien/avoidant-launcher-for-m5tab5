/*
 * clock.c -- see clock.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "clock.h"

#include <sys/time.h>
#include <time.h>

#include "esp_log.h"

#include "feckless_net.h"
#include "rtc8130.h"
#include "tab5io.h"

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
    };
    feckless_net_set_hooks(&hooks);
}
