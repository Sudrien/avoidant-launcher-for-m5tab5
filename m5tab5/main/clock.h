/*
 * clock.h -- the time, kept on the Tab5's RTC (0052).
 *
 * At boot the system clock is set from the RX8130 (rtc8130.h), which
 * keeps counting on its backup cell while the board is off. And once a
 * network is up -- a Wi-Fi join or a cable's address -- SNTP runs, and
 * each reply is written to the RTC, so the next boot, and an app run
 * without a network, start from a synced time.
 *
 * These are Avoidant's hooks (feckless_net_set_hooks()); an .elf app that
 * sets its own takes the policy over from then on, as it should.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

/* After tab5io_init() and feckless_net_start(). */
void clock_start(void);
