# Duplicitous -- the launcher

A minimal launcher for the M5Stack Tab5 (ESP32-P4): pick a `.bin` on
the microSD card or a USB drive, it is written to the `app2` slot and
booted -- in the
manner of [bmorcelli/Launcher](https://github.com/bmorcelli/Launcher),
in a few hundred lines of plain ESP-IDF.

## Using it

At power-on, if `app2` holds an app, Duplicitous boots it after three
seconds unless the screen is touched. Otherwise, or after a touch, it
lists the microSD card and the USB drive (FAT32 or exFAT, either or
both), then their folders and `.bin` files. Plugging or pulling one
updates the list. Tap a folder to open it, a
`.bin` to install and boot it; *Boot app2* at the top runs what is
already there without writing anything.

A `.bin` can be either

- an app image (starts with `0xE9`), as `idf.py build` leaves in
  `build/<project>.bin`, or
- a merged image from `0x0` (`idf.py merge-bin`, the Tab5 projects'
  `_standalone.bin`): the app is found through the image's own partition
  table at `0x8000` and only that is copied.

Either way it must be built for the ESP32-P4, fit in 4 MB, and verify
after writing; otherwise nothing is booted and the menu says why.

## Getting back

The launcher is the `factory` app. When it boots `app2` it marks it
*new*, and the bootloader (built with app rollback on) runs a new app
once; at the next reset, finding it unconfirmed, it falls back to the
launcher. So **reset returns to Duplicitous**, which boots the app again
after its countdown.

An app that confirms itself -- any ESP-IDF app built with
`CONFIG_APP_ROLLBACK_ENABLE`, which calls
`esp_ota_mark_app_valid_cancel_rollback()` -- keeps booting instead. To
get back from one, erase `otadata` from the ROM bootloader:

    esptool.py --chip esp32p4 erase_region 0xe000 0x2000

An app with an "exit to Launcher" (defeatist-music-player-for-m5tab5's,
or anything that calls `esp_ota_set_boot_partition()` on the factory
slot and restarts) lands in Duplicitous's menu directly: choosing factory
erases otadata, and Duplicitous only counts down to app2 when the
bootloader brought it back from app2 by rollback. It stays in the menu
across power cycles until something is booted.

## Partitions

| Name     | Type             | Offset   | Size    |
|----------|------------------|----------|---------|
| nvs      | data nvs         | 0x9000   | 20 KB   |
| otadata  | data ota         | 0xe000   | 8 KB    |
| launcher | app factory      | 0x10000  | 2 MB    |
| app2     | app ota_0        | 0x210000 | 4 MB    |
| ffat     | data fat         | 0x610000 | 9.9 MB  |
| coredump | data coredump    | 0xFF0000 | 64 KB   |

`ffat` and `coredump` are where the Tab5 projects' `app3M_fat9M_16MB`
layout puts them, so an app built for that layout finds its FAT, and
the FAT survives switching. Apps that expect a different table (SPIFFS,
a second OTA slot) will not find their data partitions.

## Serial console

The USB-C port carries a line console. Each reply ends with a line
starting `OK` or `ERR`. Paths start with the volume: `/sd/...` for the
card, `/usb/...` for the drive.

    version              Duplicitous <version>
    whoami               M5Stack Tab5
    echo <text>          ECHO <SHA-256 of text>: is anything else reading?
    vols                 VOL sd mounted total=<bytes> free=<bytes> |
                         VOL usb absent, one line each, then OK
    ls [dir]             D <name> | F <size> <name>, then OK; with no
                         dir, the mounted volumes (D sd, D usb)
    df [vol]             OK total=<bytes> free=<bytes>, of /sd unless /usb
    rb [-f] [-g] <dir>   receive files by YMODEM into dir (made if need be)
    sha256 <path>        OK <hex>, the file as read back from the card
    rm <path>            file or empty directory
    mkdir <path>         with parents
    install <path>       PROGRESS <done>/<total> lines, then OK
    boot                 boot app2 once
    reboot

Unasked, between commands, the console says when a card or a drive comes
or goes: `EVENT sd mounted`, `EVENT usb removed`. Never inside a reply
or a transfer. The first, just after the console starts, reports what is
already in.

### Sending files

`rb` takes a YMODEM batch, so anything that sends YMODEM works -- the web
flasher, `sz --ymodem` (lrzsz) from a terminal on the port, Tera Term,
ExtraPuTTY. Type `rb /sd/bins`, then start the send; the Tab5 asks every
second for a minute. `-g` asks for YMODEM-G, which streams without
waiting for an acknowledgement per block: much faster. It is safe
because, during a transfer, Duplicitous reads the USB port's hardware
FIFO itself rather than through ESP-IDF's driver, so USB's own flow
control holds the sender back while the card is busy (the driver would
drop the bytes instead -- see main/console.c). A sender that does not
know G will not start, so leave `-g` off for those.

A file that is already there stops the batch unless `-f` is given. Each
file is written as `<name>.part` and renamed when complete, so a
transfer that fails leaves nothing under the name. A name may contain
`/`, and lands in that subdirectory. YMODEM's CRC covers the cable, not
the card: `sha256 <path>` reads the file back, for comparing with the
sender's.

While a transfer or an install runs, the Tab5 shows what is being
written, where, and how far it has got, and the console prints no logs
(a log line would be read as protocol). That screen is the console's
only safeguard, on purpose: anyone with the cable can flash over
Duplicitous itself, so a prompt would protect nothing.

## Building

ESP-IDF 5.4.2 or later:

    idf.py set-target esp32p4
    idf.py build
    idf.py merge-bin -o duplicitous_standalone.bin

The panel, backlight, text, IO expanders, touch, USB host, the card and
the drive, and the ABI that .elf apps run on, come from
[feckless-libs-for-m5tab5](https://github.com/Sudrien/feckless-libs-for-m5tab5)'s
components, all pinned at one tag in `main/idf_component.yml`.

exFAT is built in: `cmake/exfat.cmake` runs `tools/enable_exfat.sh` at
configure time, which copies this IDF's fatfs into `components/fatfs`
with exFAT turned on (both from defeatist-music-player-for-m5tab5). Set
`TAB5_NO_EXFAT` in the environment to build FAT32 only.

Licences other than MIT: `components/usb_host_msc` is Espressif's
esp-usb MSC driver, Apache-2.0, vendored from
defeatist-music-player-for-m5tab5 with its Tab5 DMA fixes (its own
licence file and change notes inside). The exFAT fatfs is IDF's
component (Apache-2.0) around ChaN's FatFs (a BSD-style notice that
travels with it). Ark12 in the graphics handler is SIL OFL-1.1. The panel is the rev 2 Tab5's
ST7121; a rev 1 board's ILI9881C stays dark.

MIT License; see [LICENSE](../LICENSE).
