/*
 * wifiui.h -- the saved Wi-Fi networks, on the Tab5's own screen (0051).
 *
 * What Defeatist's setup page does from a phone, done here with the
 * on-screen keyboard (feckless_keyboard): a list of the networks a scan
 * heard, strongest first, chosen like the page's <select>, with "Other or
 * hidden network" for one typed by name; a password; and the network
 * tried before it is kept, in the same order the page tries it -- the
 * derived WPA2 key first where it can work, the passphrase where the
 * network is WPA3-capable. After each one, another can be added; the
 * saved list beside them can be pruned. The list is the one Avoidant and
 * the apps it runs share (feckless_net_start(), the "defeatist"
 * namespace), so a network entered here is entered for all of them.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

/* The saved networks, with Add, Forget and M5Launcher's import (0045).
 * Runs on the calling task -- the one that draws the screen -- until
 * Back. Brings the radio up only to scan and join, and takes it down
 * again on the way out if it was down on the way in. */
void wifiui_page(void);
