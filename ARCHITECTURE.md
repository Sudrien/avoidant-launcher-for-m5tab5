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
- **0010** web: step 4, files to the Tab5's card or drive. The page asks
  `version` (Duplicitous only; Launcher is told apart), sends
  `rb -g[f] <dir>`, then YMODEM-G from `web/ymodem.js` on the raw bytes
  of the same session, reads the Tab5's verdict, checks each file with
  `sha256`, and with the box ticked runs `install` and `boot`. Source:
  the firmware chosen in step 1 (a merged image; `install` takes those)
  or files from the computer. `m5tab5/test/ymodem/send.mjs` runs
  ymodem.js under Node against the receiver on the host: 20 checks pass,
  C and G -- an empty file, 100 bytes, 1 MB + 1, a subdirectory,
  refusing and replacing, refusing `../`. The page itself was run in
  headless Chromium against a fake Web Serial port whose console handed
  `rb` to the same receiver: two files sent and read back, the refusal
  reported with the Tab5's reason, `-f` replacing, `../` refused. That
  run found Send never enabled at load, fixed here. Not yet run against
  a Tab5.
- **0011** m5tab5: rb reads the USB FIFO itself, so YMODEM-G cannot
  overrun. *0010 on a Tab5* (2c75cac): `READY ymodem-g /sd/bins`, then
  `ERR bad block at 9216 bytes (0 files completed)`. Not a second
  program on the port: ESP-IDF's USB-Serial/JTAG driver empties the
  64-byte FIFO into its 4 KB ring buffer on every packet and drops what
  does not fit (`usb_serial_jtag.c`, `xRingbufferSendFromISR()`
  unchecked, in 5.5 and 6.1 alike), after USB has already acknowledged
  it -- so a stream nothing paces is lost the first time the card stalls
  on a write. 0009's README said the link "does not drop bytes"; at the
  driver it does. Now `rb` masks the receive interrupt, takes what the
  ring buffer holds, and reads the FIFO directly until the transfer
  ends; unread bytes make the hardware NAK, and the host waits. Built
  with 5.5.1 and 6.1.
- **0012** m5tab5: install erases as it writes. *On a Tab5*: a cyan
  strobe between loading the `.bin` and the install's progress bar.
  That window is `esp_ota_begin(dest, len)`, which erased the image's
  whole length in one call before the first write; IDF's DSI driver
  says an underrun can turn the panel blue, and flash erases hold the
  cache the panel's DMA competes with. Taken as the likely cause, not a
  proven one -- defeatist's "cyan flash" history is six patches against
  the wrong mechanism -- so the test is `idf.py monitor` during an
  install: `underrun happens` lines during the strobe confirm it, none
  means look elsewhere. Now `OTA_WITH_SEQUENTIAL_WRITES`: a sector
  erased ahead of each write. Not taken from the suggestion this came
  with: erasing the rest of app2 after the image (useless -- nothing
  reads past the image -- and the same stall again), `vTaskDelay()`
  between erases (the stall is inside each one), and
  `CONFIG_SPI_FLASH_AUTO_SUSPEND` (only for flash chips IDF lists; this
  one, 0x184046, is not known to be among them).
- **0013** m5tab5: phase markers for the cyan screen. *On a Tab5*: a cyan
  screen after touching to cancel the autoboot countdown -- no flash
  erase in that path, so not 0012's mechanism. Between the touch and the
  menu come flash reads (`install_present()`: app2's header, NVS), the
  card scan, and a full-screen blit (1.8 MB PSRAM to PSRAM while the
  panel scans out of PSRAM). Rather than guess -- defeatist's cyan flash
  was none of the six things patched -- `PHASE()` lines (`W (...) phase:`)
  now bracket each, so `idf.py monitor` shows which one the `underrun
  happens` lines fall inside. If none appear at all, it is not an
  underrun. Temporary; removed by the patch that fixes the cause.
- **0014** m5tab5: the panel off before every restart. *0013 on a Tab5*
  (b032786): autoboot, `touched, to the menu` at 5089 ms, the scan, the
  app2 read and the menu's blit all done by 5162 ms -- and no
  `underrun happens` line anywhere in the log, so the cyan is not the
  scanout starving. The next thing in the log is a software reset into
  app2 (the player, whose panel is up 1.9 s later). Between a reset and
  the next app's panel init nothing drives the DSI link; taken as the
  cyan. A shutdown handler now turns the backlight off and sends DISPOFF
  on every `esp_restart()`; the next app's init sends DISPON. Not yet
  confirmed: the markers stay, with a `restart: panel off` line added,
  until a Tab5 shows the gap dark.
- **0015** m5tab5: an app's "exit to Launcher" goes straight to the menu.
  The player's Exit to Launcher (`launcher_exit_to_launcher()`) sets the
  factory slot to boot and restarts; Duplicitous then counted down and
  booted the player again. IDF erases otadata when factory is chosen, so
  app2 has no record afterwards, whereas a reset out of app2 comes back
  by rollback with app2 recorded ABORTED. The countdown now runs only in
  that second case. No change needed in the apps: the same call returns
  to bmorcelli/Launcher. A side effect, on purpose: after exiting to
  Duplicitous, power-cycling stays in Duplicitous until something is
  booted again.
