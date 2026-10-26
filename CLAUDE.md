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
against the repository. The URL a session is given is for pulling only.
Local commits made to run `git format-patch` stay local.

**Patches only, never binaries.** Claude does not hand over a `.bin`,
built or merged, for testing or otherwise. The firmware that goes on a
Tab5 is what `.github/workflows/m5tab5.yml` builds from a pushed tree,
so what was tested is what is in the history -- a binary from a
session's scratch build is neither.

**Patches are cumulative.** Each applies on top of what is already here,
including patches handed over and not yet pushed. Never hand back a
rewritten copy of a file, and never reissue a corrected version of a
patch that has been handed over: send a follow-up, with the next number,
that changes what needs changing. A patch already handed over is
history.

**Handed over, not pushed, is the line.** A patch may be applied and
pushed before the session's next message, and a session cannot see
whether it was. A second file under the same number is two patches with
one name, and there is no telling which one the next patch was built on.

**A patch has one number, and it is written down once.** The number
runs on from patch to patch across sessions. It is supplied by
`--start-number`, never put in the subject:

    git format-patch --start-number 9 -1 -o out/

gives `0009-<subject>.patch`. A subject reading `0009: ...` would come
out as `0009-0009-...`; write the subject as the subject.

**Before choosing a number, look at the last one used:** the newest
entry in the series at the end of `ARCHITECTURE.md`, which every patch
adds itself to. `git log` has the subjects but not the numbers. Take
the next one; never start from a round number without checking it is
free.

**Where the numbering is now: the 0000 series, toward v0.1.0.** A
series is named for the release it leads to -- the 0000s toward v0.1.0,
the 2000s toward v0.2.0, the 3000s toward v0.3.0 -- so a patch number
says which release it went into. A point release (v0.1.1) does not
start a series. **The first patch after a tag updates this paragraph**
and opens the next series' heading in `ARCHITECTURE.md`; the tag is the
reminder, and this file is what the next session reads.

**What a patch's `ARCHITECTURE.md` entry says:** its number and
subject, one or two lines of what it does, and -- once a board has run
it -- what the log showed, with the line that proves it. A patch that
changes `sdkconfig.defaults` names the log line that proves the new
value took. Hardware results for a patch already handed over go in the
next patch's entry, not into the old one.

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
- **Run on hardware:** see the series in `ARCHITECTURE.md` for what each
  build did on a Tab5.

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

bmorcelli/Launcher 2.9.1 was offered here, with a step that talked to its console, until 0021: it takes files only by its own `flash firmware` framing, not by what this page now sends, so it went. ARCHITECTURE.md has what was learned on hardware from it.

After any reset the page makes (flash, chip read, the output view's Reset, booting app2 from step 3) it takes the Tab5's port back without a prompt: the browser keeps the permission per device (vendor, product, serial number -- the P4's USB-Serial/JTAG reports its MAC), and reacquirePort() waits for the "connect" event or a new port in getPorts(), accepting the same SerialPort object back too, in case a browser reuses it. The output view follows resets like idf.py monitor. Firefox's own hotplug test (dom/webserial/tests/mochitest/test_serial_hotplug.html) expects the permission kept, the port back in getPorts(), a connect event, and the same SerialPort object. On the Tab5 in Firefox, 0005 still prompted; a likely cause was a race (a connect event fired before the wait began was ignored), fixed in 0006 by timing from when the reset started and by accepting the same object once listed again after a disconnect. Every wait now logs "[port] ..." lines to Output (connect/disconnect events, getPorts(), how it ended): read those before guessing again. With 0006 on the Tab5 in Firefox the [port] lines showed the port handling working -- after a manual RESET, a connect event for the same object (#1) -- but also the real problem: after the flash's watchdog reset the Tab5 stayed off USB (27 s, until RESET was pressed). idf.py monitor's DTR/RTS reset (rst:0x17) brought the firmware up fine, so 0007 resets with the DTR-off RTS pulse first and uses the watchdog only if the Tab5 has not dropped off USB within 1.5 s; and any wait asks for RESET after 5 s and keeps waiting up to 2 minutes. Whether the RTS pulse resets the Tab5 out of the stub is not yet seen on hardware: look for "[port] the RTS pulse reset it".

Output is append-only (the newest 200,000 characters): every action adds a "=== <time> <action> ===" heading instead of clearing it, so a whole session -- flash, resets, [port] lines, the Tab5's boot logs -- can be copied in one piece for a report. Only its Clear button empties it. Don't add clearing back.

Release downloads from github.com have no CORS headers, so pages.yml copies each project's newest standalone .bin into `firmware/` with `firmware/index.json` (project, tag, path, size, SHA-256), daily and on push. The page checks the SHA-256 after downloading, and esptool-js checks the MD5 of what it wrote (spark-md5, pinned like esptool-js).
