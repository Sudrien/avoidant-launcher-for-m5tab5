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

/*
 * 0053: join the strongest saved network in range on a task of its own,
 * for NTP (and so the RTC), and leave it up while the menu is shown.
 * Nothing if no network is saved or the radio is already up.
 */
void clock_net_start(void);

/* Stop the boot join after the step it is in -- a join is up to about
 * thirty seconds -- and wait for it. The radio is left as it is. */
void clock_net_settle(void);

/* Settle, then the radio down: before an app gets the board. */
void clock_net_stop(void);
