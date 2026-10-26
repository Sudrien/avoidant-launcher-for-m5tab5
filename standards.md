# Standards for inclusion

What a firmware needs before the flasher lists it, and before
Duplicitous treats it as one of its own.

## Required

- **Built by a GitHub Action.** Releases come from a workflow in the
  project's own repository, not from someone's machine.

- **Both binaries in every release:**
  - the standalone image, from `idf.py merge-bin` (or the equivalent),
    written from 0x0, named `..._standalone.bin`;
  - the app-only image, for an app partition -- what Duplicitous writes
    to `app2`.

- **Checksums published with the build.** A SHA-256 for each binary,
  attached to the same release.

- **Every website named.** Any site the firmware contacts without the
  user explicitly opting in -- update checks, time servers, telemetry,
  fonts, maps, anything -- is listed in the project's documentation.

- **The 3 MB Arduino partition layout supported.** The firmware runs
  under the Arduino core's `app3M_fat9M_16MB` table:

  | Name     | Type          | Offset   | Size     |
  |----------|---------------|----------|----------|
  | nvs      | data nvs      | 0x9000   | 0x5000   |
  | otadata  | data ota      | 0xe000   | 0x2000   |
  | app0     | app ota_0     | 0x10000  | 0x300000 |
  | app1     | app ota_1     | 0x310000 | 0x300000 |
  | ffat     | data fat      | 0x610000 | 0x9E0000 |
  | coredump | data coredump | 0xFF0000 | 0x10000  |

  So the app fits in 3 MB, and any FAT data it keeps is in `ffat`.

- **If SD cards are supported (or required), USB flash drives are
  too.**

- **If external storage is supported, exFAT is.** FAT32 alone is not
  enough.

- **A library list in the firmware.** Every library it is built with,
  with its version and licence, generated during the build (not kept by
  hand) and viewable on the device.

- **The same list in the documentation.**

## Future benefits

Not required; worth having once there is a shared way to do them.

- **Wi-Fi credential sharing** between firmwares on the same Tab5, so
  networks are entered once.
- **A settings-sync API** across several devices.
