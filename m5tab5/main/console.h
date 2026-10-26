/*
 * console.h -- line commands on the USB-C port, including sending files
 * to the card.
 *
 * One command per line. Replies end with a line starting "OK" or "ERR".
 * The framing for sending bytes is Launcher's "flash firmware" one
 * (bmorcelli/Launcher src/serial_console.cpp), so a host that speaks one
 * speaks the other:
 *
 *   version                 "Duplicitous <version>", then OK
 *   whoami                  "M5Stack Tab5", then OK
 *   help                    the command list, then OK
 *   ls [dir]                one line per entry -- "D <name>" or
 *                           "F <size> <name>" -- then OK
 *   df                      "OK total=<bytes> free=<bytes>"
 *   put <size> <path>       creates/replaces path (parent directories are
 *                           made). Replies "READY <size>"; then the host
 *                           sends <size> raw bytes in chunks of 2048
 *                           (the last one shorter), waiting after each for
 *                           "ACK <received>/<size>". Ends "OK <size>".
 *                           5 s with nothing arriving gives ERR, and the
 *                           partial file is removed.
 *   rm <path>               delete a file or empty directory
 *   mkdir <path>            make a directory (and its parents)
 *   install <path>          write that .bin to app2, with
 *                           "PROGRESS <done>/<total>" lines; OK when it
 *                           verified
 *   boot                    boot app2 (once; a reset comes back here)
 *   reboot                  restart the launcher
 *
 * Paths are on the card, relative to its root ("bins/app.bin" or
 * "/bins/app.bin"); ".." is refused. The size comes before the path so
 * the path may contain spaces.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "esp_err.h"

/* "Duplicitous <version>": the build's version -- the tag CI built, or
 * `git describe` locally (esp_app_desc_t.version). */
const char *duplicitous_title(void);

/* Takes over the USB-Serial/JTAG's input and starts the console task. */
esp_err_t console_start(void);
