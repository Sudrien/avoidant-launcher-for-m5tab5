# ARCHITECTURE.md

What Avoidant is -- Duplicitous until 0032 -- and why it got that way. The rules for writing a
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
- **0016** m5tab5: .bin files that are not apps are not listed. A
  project that saved its settings as `.bin` put files on the card that
  the menu offered to install, refused only after "Install to app2?"
  with "not an ESP image". The header checks (app image or merged image,
  ESP32-P4, fits app2) are now `locate()` in install.c, shared by
  `install_from_file()` and a new `install_probe()` that reads a few
  dozen bytes; the menu's scan calls it for each `.bin` and leaves out
  what fails, saying "N .bin files here are not an ESP32-P4 app" on the
  last page and logging each with its reason (`phase: not listed: ...`).
  The console's `install` still refuses them as before.
- **0017** Storage notices for the flasher. The console tells the host,
  unasked, when a card or a drive comes or goes (`EVENT sd mounted`,
  `EVENT usb removed`), from the console task between commands, so
  never inside a reply or a YMODEM transfer; its first look reports
  what is already in. `vols` answers with both volumes' state and space.
  The page keeps a storage line in step 4 from `VOL` and `EVENT` lines,
  in any session and in "Show its output"; "Check storage" asks; Send
  asks `vols` first and refuses a volume that is not in, or too full.
  The page sees notices only while it has the port open: it does not
  hold it open between actions, because flashing needs it. Tested in
  headless Chromium against the fake port: the storage line after
  Check storage, the USB drive absent and then too full, and `EVENT`
  lines in the output view (one split across two reads) updating the
  line and the volume menu.
- **0018** A test that nothing else is reading the port. On Linux two
  programs can hold one serial port, and each USB read goes to whichever
  asks first -- a terminal or `idf.py monitor` left open takes pieces of
  the Tab5's replies (it was the first suspect for 0010's failure, wrongly
  then, but it would fail a transfer the same way). `echo <text>` answers
  `ECHO <sha256 of text>`: a hash, so it shows the Tab5 got the whole
  token, not that something echoed it. The page sends eight random
  64-hex tokens and checks each hash; any missing or damaged answer is
  reported as a shared port, with what to close. It runs before every
  Send, and as "Test the port" in step 4. In headless Chromium against
  the fake port: 8 of 8 alone; with a fake second reader taking every
  third 16-byte chunk, caught in the first round, by the button and by
  Send.
- **0019** web: a shared port caught at the first question. *0018 on a
  Tab5* (00317a7), with `idf.py monitor` also open: the two programs
  split every reply a few bytes at a time -- the page read
  `Dulicit0317a7`, `uplicitous 00317a7`, `DOK`; the monitor read the
  rest -- so `version` never came back whole, the page asked for 20 s,
  blamed Duplicitous, and never reached the echo test. Every `version`
  answer from Duplicitous ends with a line `OK`; an `OK` with no whole
  version line before it is bytes gone missing. One helper,
  `askVersion()`, now serves Send, Check storage and Test the port, and
  two such rounds stop it with the shared-port message. In headless
  Chromium with a fake reader taking single bytes at random: caught in
  about 6 s, by the button and by Send. The same log shows 0015 and 0017
  working: `phase: autoboot: not back by reset, to the menu`, and
  `EVENT sd mounted` at boot.
- **0020** web: same file skipped, different file replaced. *0019 on a
  Tab5* (8bdb166): with the monitor still open the first try stopped
  after `version` (the page got an `E` of the first `ECHO`); the second,
  alone, passed 8 of 8, `vols` showed the card (87 GB free), and `rb`
  refused `lothesome_v0.3.0-pre1_..._standalone.bin` as already there
  -- from an earlier send that had worked. Asked: check the SHA-256,
  and replace without asking if it differs. The page now asks `sha256`
  for each file before sending: the same hash is reported "already
  there, identical" and not sent (install and boot still follow); any
  other answer sends it, with `rb -gf`, and the read-back reports "new"
  or "replaced". The free-space check counts only what is sent. The
  "Replace files that are already there" box is gone. `rb` without
  `-f` still refuses for terminals. In headless Chromium: two files
  sent, sent again (nothing sent), one changed (only it sent, and the
  new one on the volume).
- **0021** Launcher off the flasher. bmorcelli/Launcher takes files only
  by its own `flash firmware <name> <size>` framing (READY, raw bytes,
  ACK per 2 KB), not by `rb`/YMODEM, `sha256` or `echo`, so step 3 (talk
  to Launcher) and the pinned download in pages.yml went; step 4 is now
  step 3. A Tab5 still running Launcher answers `version`, and the page
  says it cannot take files. What was learned from it, kept here from
  the CLAUDE.md notes this removed: Launcher 2.9.1's
  `Launcher-m5stack-tab5.bin` is a whole image from 0x0 (0xFF to 0x2000,
  chip ID 18), 1603072 bytes, SHA-256 506b02a4...b0a8, its table
  nvs/otadata/app0 at 0x10000/coredump, and it reworks the partition
  table as it installs apps; its console (src/serial_console.cpp) is
  always on, prints "Press the button to enter the Launcher!" every
  500 ms while waiting to boot an app, and answers `version` with
  "Launcher <version>". On hardware, its first boot after other
  firmware logged `E (278) M5GFX: M5Tab5 display panel was not detected`
  and ran dark until the next reset -- likely the I/O expanders keeping
  the previous firmware's state through a chip-only reset.
  Duplicitous was already in deps.yml's list (since 0002); with
  m5tab5/dependencies.lock now committed, `tools/depcheck.py m5tab5`
  reads it: the feckless components checked against their tags here,
  the registry ones unreachable from a session. The vendored
  `components/usb_host_msc` is not in the lock, so it is not checked.
- **0022** CLAUDE.md: what was learned, and what is planned. A "Learned
  on hardware" list under m5tab5/ (the USB-Serial/JTAG driver dropping
  bytes, the two kinds of cyan, factory erasing otadata, a shared port,
  the exFAT card, the temporary markers); a summary of step 3's
  conversation with the console under web/; and "Planned, not built" --
  first, "flash standalone" and "flash with the Duplicitous launcher"
  side by side -- with what has not yet been seen on a Tab5.
- **0023** m5tab5: .elf apps, run without a restart. Built against
  feckless-abi-for-m5tab5 v0.2.0, which now pins feckless_drivers and
  feckless_graphics_handler for this firmware (main's manifest names it
  in their place). The menu lists `.elf` files beside `.bin`s, in green,
  if the header says 32-bit RISC-V shared object, 4 MB or less
  (`elfrun.c`). Tapping one asks "Run?", reads it into PSRAM, and hands
  it to `feckless_abi_run()` on the main task. The app's `app_main()`
  runs there and initialises the board again, which the ABI's v0.5.10
  and v0.2.8 libraries allow. Its USB classes register late, and the
  port's power is cycled so the card reader and drive enumerate again
  with them. The console and storage keep running underneath. An ELF
  that will not load (a symbol outside the ABI, another ABI version)
  brings the menu back with the log naming the symbol; an app whose
  `app_main()` returns leaves the board to its own tasks, and the main
  task deletes itself. sdkconfig.defaults gains esp_elf's PSRAM load,
  memory protection off, the 256 KB L2 and `CONFIG_UAC_FREQ_NUM_MAX=8`
  for an app's USB microphone: `rm sdkconfig`. Built with ESP-IDF 5.5.1
  and the component manager off, with the drivers' and graphics
  handler's unreleased commits; the image is 1,269 KB, inside the
  launcher's 2 MB. Not on a Tab5.
- **0024** m5tab5: feckless-abi v0.2.1. 0023's v0.2.0 named
  feckless-graphics-handler v0.2.8, which was tagged without the change
  that lets lcd_init() and gfx_init() run a second time: a .elf app
  would have stopped at its lcd_init(). v0.2.1 names v0.2.9, which has
  it. Touches main/idf_component.yml: the lock re-resolves (0023's was
  not committed; this one should be). Not built.
- **0025** m5tab5: feckless-abi v0.2.2. *0024 on a Tab5*: the card
  mounted, Lothesome's .elf listed and ran, and esp_elf refused it --
  `E ELF: Can't find common feckless_abi_v1`, `esp_elf_relocate: -88`.
  The ABI's table registered the version symbol under the name
  "FECKLESS_ABI_SYMBOL" (an unexpanded stringize); v0.2.2 registers it
  as feckless_abi_v1. The menu came back as it should after a failed
  load. Touches main/idf_component.yml: commit the lock. Not built.
- **0026** m5tab5: a `===` line first in the log. `=== Duplicitous ===`,
  the version, IDF, build date and time, and the feckless ABI version,
  as Lothesome's 0005 has it -- a warning, so the console's level lets
  it through. And feckless-abi v0.2.3, whose check deletes an ELF that
  fails it. *0025 on a Tab5*: `feckless_abi_v1` was found, and
  Lothesome's .elf refused on `Can't find common dsps_fft2r_fc32_arp4_`
  -- esp-dsp not linked into that ELF, which a failed build had left
  behind. Built with ESP-IDF 5.5.1 and the component manager off.
- **0027** m5tab5: feckless-abi v0.2.4. *0026 on a Tab5*: the `===`
  line came first (`=== Duplicitous === v0.1.0-8-g77633e5, IDF
  v6.1-829-g9a97f6c54ec, ... feckless ABI v1`), the card mounted, and
  Lothesome's .elf -- a good build -- was refused again on
  `Can't find common dsps_fft2r_fc32_arp4_`. elf_loader 2.0.0 looks a
  relocation against the ELF's own esp-dsp entry point up in this
  firmware's table. Nothing here changes: v0.2.4 links apps with
  -Bsymbolic so they make no such relocation, and checks for it. The
  .elf must be rebuilt against v0.2.4. Not built.
- **0028** m5tab5: the gear, and a settings page under it. A gear at the
  top right of the menu's header (drawn: a hub, eight teeth and a hole)
  opens it. First the console log level -- None, Error, Warn, Info,
  Debug, the one in force lit -- applied at once, kept in NVS
  (`duplicitous`/`loglevel`, `settings.c`), and set by `console_start()`
  in place of the fixed WARN; it covers a running .elf app's logs too,
  and a transfer still silences everything while it runs. Under it the
  libraries the build was made from, with version and licence:
  `cmake/libversions.cmake`, Lothesome's, writing libversions.h at
  configure time from the lock, the vendored `components/` (usb_host_msc
  1.3.0) and the Ark Pixel font. And feckless-abi v0.2.5, whose apps
  carry their own esp_app_desc_t. *0027 on a Tab5*: Lothesome's .elf
  ran from the card under Duplicitous, at 40 fps; only its warnings
  reached the console (the WARN this patch makes a setting), and its
  `===` line gave Duplicitous's version as its own (v0.2.5's fix).
  Built with ESP-IDF 5.5.1 and the component manager off: 1,272 KB, 15
  libraries listed. Not on a Tab5.
- **0029** web, m5tab5: apps on the flasher. pages.yml fetches each
  project's newest release's `.elf` beside its `_standalone.bin`, and
  lists it in firmware/index.json with `kind: elf` and the feckless ABI
  version it refers to -- if it is an app: a 32-bit RISC-V shared object,
  as elfrun.c asks, so a debug `build/<project>.elf` is left out with a
  warning. Step 3 gains a third source, "An app for Duplicitous", which
  fetches the .elf, checks its SHA-256 against the index and sends it, to
  `apps` unless another folder was typed. Any .elf sent, published or
  from the computer, is checked first: an app, and for the ABI the
  Tab5's Duplicitous runs, which `version` now says on a second line,
  `ABI feckless v1` (console.h). An older Duplicitous says nothing; the
  page sends and says so. standards.md describes the optional .elf.
  Tested: the index script on a release folder holding Lothesome's app
  ELF, its debug ELF and a .bin (the app listed with abi 1, the debug
  ELF left out); the page in headless Chromium against that index (step
  1 the .bin alone, step 3 the app, choosing it moves the folder to
  `apps`, no page errors); the ABI test on the same three files in
  Node; the firmware built with ESP-IDF 5.5.1 and the component manager
  off. Not on a Tab5, and not deployed.
- **0030** m5tab5: the feckless libraries from feckless-libs-for-m5tab5
  v0.2.0. `main/idf_component.yml` names feckless_abi, feckless_drivers,
  feckless_graphics_handler and feckless_storage_handler there, each by
  `path:`, all at v0.2.0, in place of feckless-abi v0.2.5 (which pinned
  the drivers and graphics handler) and feckless-storage-handler v0.1.0.
  The code is the same: v0.2.0 imported those tags unchanged, so this is
  still ABI v1 and Lothesome's .elf runs as before. The READMEs,
  standards.md, CLAUDE.md and the flasher's link name the new
  repository. Built with ESP-IDF release/v6.1 (9a97f6c) and the
  component manager off, against feckless-libs at v0.2.0 (2edea9f):
  `duplicitous.bin binary size 0x135030 bytes`. `dependencies.lock` not
  updated. Not on a Tab5.
- **0031** m5tab5: feckless-libs v0.3.0, which takes what was vendored
  here. `main/idf_component.yml` names v0.3.0 for the four feckless
  components, and adds fatfs (exFAT, in place of cmake/exfat.cmake,
  cmake/idfcopy.cmake and tools/enable_exfat.sh, deleted, with the
  .gitignore line for components/fatfs), usb_host_msc (the same patched
  1.3.0 that was components/usb_host_msc, deleted), usb_host_uac (the
  drivers' registry pin until v0.2.0) and feckless_abi_storage, the ABI's
  storage section: .elf apps that call feckless_abi_require_storage() get
  the card and the drive, and FatFs, from Duplicitous. TAB5_NO_EXFAT is
  CONFIG_FECKLESS_FATFS_EXFAT=n now. The README, CLAUDE.md and
  libversions.cmake's notes follow; the planned item for vendored
  components in the dependency check goes, there being none. Built with
  ESP-IDF release/v6.1 and the component manager off, against
  feckless-libs v0.3.0: `fatfs (feckless): exFAT`, no warnings,
  duplicitous.bin 0x137290 bytes, feckless_abi_storage_v1 in it.
  `dependencies.lock` not updated. After applying: delete
  m5tab5/components/fatfs and build/, and `rm sdkconfig`. Not on a Tab5.
- **0032** Duplicitous is Avoidant, and the repository
  avoidant-launcher-for-m5tab5: two Tab5 projects starting with D were
  one too many. The project is `avoidant`, so the image is avoidant.bin
  and the releases' files `avoidant_<tag>_m5tab5*`; the console answers
  `version` with `Avoidant <version>`, and the screen's title, the log's
  `===` line and the licences page say Avoidant. The flasher's Pages
  address is https://sudrien.github.io/avoidant-launcher-for-m5tab5/,
  and its links, pages.yml's and deps.yml's project lists and
  depreport.py's link follow the repository. Kept: the NVS namespace
  `duplicitous`, so a Tab5 updated from Duplicitous keeps its settings
  and its record of app2; and the flasher still takes a `Duplicitous
  <version>` answer as this launcher's, for a Tab5 not yet updated.
  This file's entries before this one keep the old name. Built with
  ESP-IDF release/v6.1 and the component manager off: avoidant.bin
  0x1372c0 bytes, holding "Avoidant %s" and the namespace "duplicitous".
  The page's script parses (node --check); its name tests take
  "Avoidant v0.2.0" and "Duplicitous v0.1.9" as the launcher, "Launcher
  2.9.1" only as Launcher. Not on a Tab5, and the page not deployed.
- **0033** Aimless Moving Map is Errant Moving Map
  (errant-moving-map-for-m5tab5, its 0042), and the dependency check's
  paths. pages.yml's and deps.yml's project lists and standards.md's
  example name it. deps.yml also had three projects' locks at the top of
  the repository, where none is since each moved its firmware into
  m5tab5/ -- Lothesome's 0044, Defeatist's 8059, Errant's 0039 -- so the
  weekly report read nothing for them; all five are `<repo>:m5tab5`
  now. And CLAUDE.md's planned "Flash with the Avoidant launcher" named
  the NVS namespace `minilauncher`; it is `duplicitous` (install.c).
  Run here: tools/depcheck.py on each of the four from its m5tab5/,
  Defeatist's dependencies.lock.idf6 found by itself -- each read, its
  git dependencies up to date (the registry is not reachable from a
  session, so those were not checked).
