# duplicitous-for-m5tab5

Two things for the M5Stack Tab5 (ESP32-P4):

- **[m5tab5/](m5tab5/)** -- Duplicitous, a minimal launcher: pick a
  `.bin` on the microSD card or a USB drive (FAT32 or exFAT) and it is
  written to the `app2` slot and booted. A console on the USB-C port
  sends files to either.
- **[web/](web/)** -- a web flasher, over the Tab5's USB-C port.

**Open the flasher: <https://sudrien.github.io/duplicitous-for-m5tab5/>**

## The flasher

Choose a firmware, then press Flash: the browser asks for the Tab5's
serial port, the page asks you to confirm, and
[esptool-js](https://github.com/espressif/esptool-js) writes it. Works
in Chrome, Edge, and Firefox 151 or later on desktop; Safari has no
Web Serial.

The firmware is a standalone image (the `_standalone.bin` each project
releases, written from 0x0) -- either the newest release of one of the
Tab5 projects (Duplicitous among them), which the deploy copies into the site daily, or a
file from your computer. Flashing erases the Tab5's saved settings.

[Launcher](https://github.com/bmorcelli/Launcher) 2.9.1's Tab5 image is
on the list too, pinned to that release. Once it is flashed, a third
step checks that Launcher answers on its serial console, asking it
`version`, `whoami` and `partitions`.

With Duplicitous on the Tab5, step 4 sends files to its microSD card or
USB drive instead -- the firmware chosen in step 1, or files from your
computer -- by YMODEM over the same port, reads each one back to check
its SHA-256, and can install a `.bin` to `app2` and boot it. It shows
what storage the Tab5 has ("Check storage" asks), kept current by the
Tab5's own notices whenever the page has the port open, and checks
there is room before sending.

The port and chip tests (reset into the ROM bootloader, read the chip,
reset back) are still on the page, folded under "Port and chip tests".

## Dependency report

Every Monday, `.github/workflows/deps.yml` runs `tools/depcheck.py` on
each Tab5 project's `dependencies.lock` -- against the Espressif
registry, the projects' git remotes and ESP-IDF's tags -- and keeps one
open issue here, "Dependency updates", saying what is newer. It changes
nothing in the projects. Locally, from a project's directory:

    python3 tools/depcheck.py m5tab5

MIT License; see [LICENSE](LICENSE).
