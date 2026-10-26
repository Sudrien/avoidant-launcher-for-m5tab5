# CLAUDE.md

How to work on this repository. Read it before writing a patch.

Two parts: `m5tab5/` is Duplicitous, the launcher firmware for the Tab5;
`web/` is the web flasher, published to GitHub Pages by
`.github/workflows/pages.yml`. `tools/` is the dependency check that
`.github/workflows/deps.yml` runs weekly.

## Patches

Claude creates `git am`-able patches authored as
`Claude <noreply@anthropic.com>`, and presents each one as soon as it
exists -- a session can end with patches stuck behind it.

Claude must not commit to, push to, branch on, or open a pull request
against the repository. Local commits made to run `git format-patch`
stay local.

**Patches are cumulative.** Each applies on top of what is already here.
Never reissue a corrected version of a patch that has been handed over:
send a follow-up with the next number. The number comes from
`--start-number`, never from the subject:

    git format-patch --start-number 3 -1 -o out/

Before choosing a number, look at the last one used (`git log`) and take
the next.

## m5tab5/

ESP-IDF (>= 5.4.2), on `feckless_graphics_handler` and `feckless_drivers`
from defeatist-music-player-for-m5tab5's family of components.

- **Every dependency is pinned to a tag.**
- **The Espressif registry is not reachable from a session.** A session
  can still build by fetching the registry components' sources from
  GitHub (esp-bsp, esp-iot-solution, esp-usb) into a scratch component
  directory and building with `IDF_COMPONENT_MANAGER=0`; a patch says
  whether it was built that way, or not at all. Neither way updates
  `dependencies.lock`, so a patch that touches an `idf_component.yml`
  says so.
- **Any change to `sdkconfig.defaults` needs `rm sdkconfig`** before the
  next build. Say so in the commit message.
- **Not yet run on hardware**, as of the first patch.

## web/

### Confirmed on hardware

Firefox (151+, desktop) has been tested against a real Tab5: the port prompt was granted and the page reported USB vendor 0x303A, product 0x1001 -- the ESP32-P4's built-in USB-Serial/JTAG, not a separate USB-UART bridge. So:

- Firefox is a supported browser, not a "supposedly". Don't add Chrome-only workarounds or warnings for it.
- The ROM bootloader is on that same port. esptool-js can normally reset into download mode itself (DTR/RTS through the USB-Serial/JTAG reset logic). If the running firmware has disabled or repurposed that peripheral, auto-reset fails; the fallback is telling the user to hold BOOT while resetting.
- On reset the USB device drops off the bus and comes back. Treat a lost port as expected after flashing (and after the reset into download mode): close cleanly, and be ready to ask for the port again rather than erroring out.
- Getting back out of the bootloader: esptool-js 0.7.0's `after('hard_reset')` only sets RTS false and never true, so over USB-Serial/JTAG it resets nothing and the Tab5 stays in the bootloader (seen on hardware). Use the P4 target's `watchdogReset(loader)`, with a DTR-off RTS pulse as the fallback. Check this again before trusting `after()` in a newer esptool-js.
- Tested end to end on that Tab5 in Firefox: jsDelivr served the pinned esptool-js 0.7.0 (the sha384 passed), the reset into the bootloader worked without BOOT, the stub ran, flash ID 0x184046 read as 16MB, the chip is an ESP32-P4 rev v1.3 with a 40 MHz crystal, and the watchdog reset brought the installed firmware back up.

### What gets flashed

Each project's release has two .bins: `..._standalone.bin`, from `idf.py merge-bin`, written at 0x0, and the app alone, for the app partition. The page flashes only the standalone one, checked first: on the P4 the bootloader is at 0x2000, so the file is 0x2000 bytes of 0xFF and then an image header (0xE9, chip ID 18 at byte 12). An app-only .bin starts with 0xE9 and is refused.

The merged image runs from 0x0 to the end of the app, so it overwrites NVS (0x9000) -- settings are lost -- and stops short of the FAT partitions (0x610000 in the OTA layouts), which survive. The four projects use two partition layouts (two-OTA-slot plus FAT, and factory only); the image carries its own table, so switching between projects is fine.

pages.yml also fetches pinned images from other repos, listed as "repo tag file" in its fetch step; for now bmorcelli/Launcher 2.9.1's Launcher-m5stack-tab5.bin. Checked: it is a whole image from 0x0 like a _standalone.bin (0xFF to 0x2000, chip ID 18), 1603072 bytes, SHA-256 506b02a4...b0a8, its table nvs/otadata/app0 at 0x10000/coredump. Launcher reworks the partition table itself as it installs apps, so after it the projects' layouts no longer apply until one of their standalone images is flashed again.

After Launcher is flashed (or while it is the chosen firmware), step 3 talks to its serial console (src/serial_console.cpp: one command per line, always on from early boot): `version` must answer "Launcher <version>", `whoami` should answer "M5Stack Tab5", and `partitions` is shown. All three only read. It ignores the "Press the button to enter the Launcher!" banner Launcher prints every 500 ms while waiting to boot an app. The test reuses the one Espressif port the browser already allows (the Tab5 back after its reset), else prompts. Tested against a fake port only, not yet on the Tab5 -- in particular whether opening the port with Web Serial resets the chip (it would show as the port going away).

After any reset the page makes (flash, chip read, Launcher "reboot", the output view's Reset) it takes the Tab5's port back without a prompt: the browser keeps the permission per device (vendor, product, serial number -- the P4's USB-Serial/JTAG reports its MAC), and reacquirePort() waits for the "connect" event or a new port in getPorts(), accepting the same SerialPort object back too, in case a browser reuses it. The output view follows resets like idf.py monitor. Firefox's own hotplug test (dom/webserial/tests/mochitest/test_serial_hotplug.html) expects the permission kept, the port back in getPorts(), a connect event, and the same SerialPort object. On the Tab5 in Firefox, 0005 still prompted; a likely cause was a race (a connect event fired before the wait began was ignored), fixed in 0006 by timing from when the reset started and by accepting the same object once listed again after a disconnect. Every wait now logs "[port] ..." lines to Output (connect/disconnect events, getPorts(), how it ended): read those before guessing again. With 0006 on the Tab5 in Firefox the [port] lines showed the port handling working -- after a manual RESET, a connect event for the same object (#1) -- but also the real problem: after the flash's watchdog reset the Tab5 stayed off USB (27 s, until RESET was pressed). idf.py monitor's DTR/RTS reset (rst:0x17) brought Launcher up fine, so 0007 resets with the DTR-off RTS pulse first and uses the watchdog only if the Tab5 has not dropped off USB within 1.5 s; and any wait asks for RESET after 5 s and keeps waiting up to 2 minutes. Whether the RTS pulse resets the Tab5 out of the stub is not yet seen on hardware: look for "[port] the RTS pulse reset it".

Seen on hardware with idf.py monitor: Launcher 2.9.1's first boot after other firmware logged "E (278) M5GFX: M5Tab5 display panel was not detected" and ran with the screen dark (console fine); the next reset found the display. Likely the board's I/O expanders keeping the previous firmware's state through a chip-only reset; a Launcher/M5GFX matter, not the flash (which verified). Confirmed on hardware: the extra reset brought the display up. Step 3's "Restart Launcher" is the workaround.

Output is append-only (the newest 200,000 characters): every action adds a "=== <time> <action> ===" heading instead of clearing it, so a whole session -- flash, resets, [port] lines, the Tab5's boot logs -- can be copied in one piece for a report. Only its Clear button empties it. Don't add clearing back.

Release downloads from github.com have no CORS headers, so pages.yml copies each project's newest standalone .bin into `firmware/` with `firmware/index.json` (project, tag, path, size, SHA-256), daily and on push. The page checks the SHA-256 after downloading, and esptool-js checks the MD5 of what it wrote (spark-md5, pinned like esptool-js).
