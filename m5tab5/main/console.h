/*
 * console.h -- line commands on the USB-C port, including sending files
 * to the card.
 *
 * One command per line. Replies end with a line starting "OK" or "ERR".
 * Files go to the card or the drive by YMODEM (ymodem.h), which any
 * terminal with sz, Tera Term or ExtraPuTTY can send, and so can the web
 * flasher:
 *
 *   version                 "Duplicitous <version>", then OK
 *   whoami                  "M5Stack Tab5", then OK
 *   help                    the command list, then OK
 *   vols                    "VOL sd mounted total=<bytes> free=<bytes>" or
 *                           "VOL usb absent", one line each, then OK
 *   ls [dir]                one line per entry -- "D <name>" or
 *                           "F <size> <name>" -- then OK. With no dir,
 *                           the mounted volumes: "D sd", "D usb".
 *   df [vol]                "OK total=<bytes> free=<bytes>", of /sd
 *                           unless /usb is named
 *   rb [-f] [-g] <dir>      make dir, reply "READY ymodem[-g] <dir>, ...",
 *                           then receive a YMODEM batch into it: -g asks
 *                           for YMODEM-G (streaming), -f replaces files
 *                           that are there (otherwise one that is cancels
 *                           the batch). Ends "OK <files> files, <bytes>
 *                           bytes" or "ERR <why> (<n> files completed)".
 *                           Logs are off until it ends.
 *   sha256 <path>           "OK <64 hex digits>", the file as read back
 *   rm <path>               delete a file or empty directory
 *   mkdir <path>            make a directory (and its parents)
 *   install <path>          write that .bin to app2, with
 *                           "PROGRESS <done>/<total>" lines; OK when it
 *                           verified
 *   boot                    boot app2 (once; a reset comes back here)
 *   reboot                  restart the launcher
 *
 * Unasked, between commands: "EVENT sd mounted", "EVENT usb removed" and
 * so on, when a card or a drive comes or goes (the first look, about a
 * quarter of a second after the console starts, reports what is already
 * mounted). Never inside a reply or a transfer.
 *
 * Paths start with the volume: /sd/... for the card, /usb/... for a USB
 * drive (the leading slash may be left off); ".." is refused.
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
