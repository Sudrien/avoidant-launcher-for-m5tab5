# ARCHITECTURE.md

What Duplicitous is, and why it got that way. The rules for writing a
patch are in `CLAUDE.md`; this file is the record. Entries are in the
order they were written. Where a later one contradicts an earlier one,
the later one is what happened.

## The 0000 series, toward v0.1.0

- **0001** Duplicitous: a minimal launcher for the Tab5. The menu, the
  install into `app2`, booting it once by way of app rollback, and the
  line console with Launcher's READY/ACK framing for `put`. Its own SD
  mount (`main/sd.c`).
- **0002** Move the firmware to m5tab5/, and the web flasher into web/.
- **0003** standards.md: what a firmware needs to be included.
- **0004** standards.md: binaries named for themselves, not the board.
- **0005** standards.md: ESP-IDF 6.1, what inclusion gets you, and a
  request form.
- **0006** m5tab5: build and release workflow, like
  unprofitable-mahjong's. The version shown is the build's.
  *On a Tab5* (43caad9, ESP-IDF 6.1): bootloader, panel (ST7121) and
  touch (ST7123 at 0x55) came up; the card did not mount --
  `diskio_sdmmc: sdmmc_read_blocks failed (0x104)` four times, then
  `vfs_fat_sdmmc: failed to mount card (1)`, and on the retry
  `gpio: conflict found for GPIO[42]`. An exFAT card under a FAT32-only
  FatFs, and `sd.c`'s retry not releasing the slot.
- **0007** m5tab5: USB drives and exFAT, through
  feckless_storage_handler. exFAT from the player's
  `cmake/exfat.cmake`; `sd.c` replaced by the storage handler (`/sd`,
  `/usb`); USB host from feckless_drivers; the player's patched
  `usb_host_msc`; console paths start with the volume.
  *On a Tab5* (6f6ae47, ESP-IDF 6.1): `tab5_storage: microSD mounted
  at /sd` -- SDHC/SDXC, 236012 MB, 40000 kHz, 4-bit. USB drive not yet
  tried.
- **0008** CLAUDE.md: numbered patches in series, like
  defeatist-music-player-for-m5tab5's; no binaries handed over; this
  file, with the series so far.
- **0009** m5tab5: files to the card by YMODEM, and sha256. `put`
  (Launcher's READY/ACK framing) is replaced by `rb [-f] [-g] <dir>`, a
  YMODEM batch receiver written from Forsberg's reference (`ymodem.c`,
  no IDF in it), so terminals' `sz`, Tera Term and the web flasher can
  all send; `-g` is YMODEM-G. `sha256 <path>` reads a file back (vendored
  SHA-256: Mbed TLS 4 dropped the legacy header). No overwriting without
  `-f`; `.part` until complete. A transfer screen on the Tab5, and the
  autoboot countdown stops when one starts. Considered and not taken:
  an on-device approval prompt (the cable can reflash anyway), ZMODEM
  (lrzsz is GPL; NuttX's port is ~3,700 lines), Kermit (no browser
  library), USB mass storage (for later: it takes the console off the
  port). `m5tab5/test/ymodem/run.sh` runs the receiver on the host
  against lrzsz's sz: 15 checks pass -- YMODEM and -G, an empty file,
  1 MB + 1, refusing and replacing, a transfer cut at 500 KB leaving
  nothing behind.
