# duplicitous-for-m5tab5

A minimal launcher for the M5Stack Tab5 (ESP32-P4): pick a `.bin` on
the microSD card, it is written to the `app2` slot and booted -- in the
manner of [bmorcelli/Launcher](https://github.com/bmorcelli/Launcher),
in a few hundred lines of plain ESP-IDF.

## Using it

At power-on, if `app2` holds an app, Duplicitous boots it after three
seconds unless the screen is touched. Otherwise, or after a touch, it
lists the card's folders and `.bin` files. Tap a folder to open it, a
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

The USB-C port carries a line console that also sends files to the card.
Each reply ends with a line starting `OK` or `ERR`.

    version              Duplicitous <version>
    whoami               M5Stack Tab5
    ls [dir]             D <name> | F <size> <name>, then OK
    df                   OK total=<bytes> free=<bytes>
    put <size> <path>    READY <size>; then <size> raw bytes in 2048-byte
                         chunks (the last shorter), waiting after each for
                         ACK <received>/<size>; ends OK <size>
    rm <path>            file or empty directory
    mkdir <path>         with parents
    install <path>       PROGRESS <done>/<total> lines, then OK
    boot                 boot app2 once
    reboot

The `put` framing is the one Launcher's `flash firmware` uses: one chunk
in flight, so a slow driver cannot stall the stream. A file is written
as `<path>.part` and renamed when complete; 5 s with no bytes gives ERR
and removes it. Paths are relative to the card's root, and the size
comes first so a path may contain spaces.

## Building

ESP-IDF 5.4.2 or later:

    idf.py set-target esp32p4
    idf.py build
    idf.py merge-bin -o duplicitous_standalone.bin

The panel, backlight, text, IO expanders and touch come from
[feckless-graphics-handler-for-m5tab5](https://github.com/Sudrien/feckless-graphics-handler-for-m5tab5)
and
[feckless-drivers-for-m5tab5](https://github.com/Sudrien/feckless-drivers-for-m5tab5),
pinned in `main/idf_component.yml`. The panel is the rev 2 Tab5's
ST7121; a rev 1 board's ILI9881C stays dark.

MIT License; see [LICENSE](LICENSE).
