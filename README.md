# RT4K Serial Bridge (ESP32-S3)

Bridges a RetroTINK-4K's USB serial console over WiFi. The ESP32-S3 acts as
a USB host for the RT4K's onboard FT232R chip, and exposes a web UI (served
from the ESP32 itself) for a live terminal, profile management, and device
configuration — no PC or USB cable required once set up.

## Features

- **Live terminal** — WebSocket-based console to the RT4K's serial
  interface.
- **Profile management** — browse the RT4K's SD card `/profile` folder,
  load/download/upload/delete `.rt4` profiles from the browser.
- **WiFi setup** — connects to a saved network on boot, or falls back to
  hosting its own access point (`TinkEsp32`) for first-time setup. Includes
  network scanning (cached at boot and on demand) and a persistent,
  configurable hostname reachable via mDNS (`http://<hostname>.local`).
- **Serial config** — switch baud rate from the web UI: 2,000,000 (the
  RT4K's default) or 115,200.
- **RT4K firmware updates** — pick the RetroTINK firmware `.zip` in the
  browser; the bridge uploads the right files for the connected model to the
  SD card, installs them, and reports progress live. No pulling the card.
  See [API](#api).
- **OTA updates** *(optional, per board)* — flash firmware and the web UI
  together from the browser instead of over USB, on boards built with OTA
  support. See [API](#api) and [Adding a new board](#adding-a-new-board).
- **Self-contained** — the web UI is a static Svelte app served straight
  from the ESP32's onboard flash (LittleFS); nothing is fetched externally
  once the AP-mode fallback UI is loaded.

## Hardware

Board: ESP32-S3-DevKitC-1 (N16R8 — 16MB flash / 8MB PSRAM). Requires a chip
with native USB-OTG host support (ESP32-S3, -S2, or -P4); the original
ESP32 cannot do USB host and won't work here.

RetroTINK-4K: firmware 1.80 or newer.

Most ESP32-S3-DevKitC-1 boards expose two USB-C ports:

- One is wired to the onboard CP2102/CH340 UART bridge, used for
  flashing/`Serial` logging. It's commonly labeled `TTL` or `COM` on the
  silkscreen.
- The other is wired directly to the S3's native USB peripheral, and is
  the one that must connect to the RT4K's FT232R port (via USB host, i.e.
  a USB-A to USB-C/micro cable as appropriate — the RT4K is the USB
  device here, the ESP32 is the host). It's commonly labeled `USB` or
  `OTG`. See [Powering the RT4K](#powering-the-rt4k) for getting power to
  the RT4K on this path.

Board silkscreens and revisions vary, so if you're not sure which port is
which, flash once with a cable in each port in turn and check which one
the Serial Monitor / `pio run -t upload` actually uses — that's the UART
port. The other one goes to the RT4K.

### Powering the RT4K

The RT4K draws up to about 2A, which is more than the DevKitC-1's 5V rail
is built to pass, so power the two devices separately rather than running
the RT4K off the ESP32.

Many ESP32-S3-DevKitC-1 boards have a solder jumper on the bottom, next to
the USB-OTG port, that connects that port's VBUS to the board's 5V rail.
It ships open, and bridging it is the usual advice for powering a USB
device from the board. **Don't bridge it for the RT4K.** At 2A that solder
joint and the board's thin traces carry far more current than they're
rated for, which gets you voltage sag at the RT4K (random brownouts,
resets and failed transfers), a hot board, and at worst scorched traces,
a damaged regulator, or a dead DevKitC. Leave it open and the OTG port
carries data only, which is all this project needs from it.

So give each device its own supply:

- **ESP32:** a 5V USB supply into its UART/`COM` port, which also keeps
  `Serial` logging available.
- **RT4K:** its own power, combined with the ESP32's data onto the RT4K's
  USB-C port by a power-and-signal splitter such as Waveshare's
  [USB-C Power Splitter](https://www.waveshare.com/usb-c-power-splitter.htm)
  (three USB-C ports: data in from the ESP32's OTG port, 5V in from the
  supply, combined out to the RT4K).
- **Cable:** use a good USB-C cable between the splitter and the RT4K.
  Thin or charge-only cables drop enough voltage at 2A to cause the same
  brownouts the jumper does.


## Quick start: flashing from a release

No compiling needed for this — grab prebuilt files from the
[Releases page](../../releases).

Each release has files named for the board they're for, e.g. for
`esp32-s3-devkitc1-n16r8`:

- `esp32-s3-devkitc1-n16r8-blank-board-init.bin`
- `esp32-s3-devkitc1-n16r8-update.bin`
- `esp32-s3-devkitc1-n16r8-ota-update.bin`

Connect the board via its **UART/flashing** USB port (see Hardware above
for which port that is).

**Preferred: [esptool-js](https://espressif.github.io/esptool-js/)** —
Espressif's official browser-based flashing tool, no install required
(Chrome/Edge/Opera only — it needs Web Serial, which Firefox and Safari
don't support). Connect, add one row per file below at its offset, and
flash:

- **First time on a factory-blank board** — one row: `0x0` →
  `esp32-s3-devkitc1-n16r8-blank-board-init.bin`. It's the complete image,
  so don't use it to update a running board — it resets saved settings.
- **Updating a board that's already running** — just one row: `0xe000` →
  `esp32-s3-devkitc1-n16r8-update.bin`. This doesn't touch its saved
  WiFi/hostname/baud settings.

**Alternative: the `esptool` CLI**, if you'd rather script it or Web
Serial isn't an option:

```sh
pip install esptool
```

```sh
# first time on a factory-blank board
esptool --chip esp32s3 --port <PORT> write-flash 0x0 esp32-s3-devkitc1-n16r8-blank-board-init.bin

# updating a board that's already running
esptool --chip esp32s3 --port <PORT> write-flash 0xe000 esp32-s3-devkitc1-n16r8-update.bin
```

Replace `<PORT>` with the board's serial port: `COM3`-style on Windows,
`/dev/ttyUSB0` or `/dev/cu.usbserial-...` on Linux/macOS (omit `--port` and
`esptool` will try to auto-detect it).

Either way, power-cycle the board once flashing finishes. On a blank
board it comes up hosting its own `TinkEsp32` WiFi network (password
`12345678`) — connect to it and open `http://192.168.4.1` to join it to
your real network. The `0xe000` offset is current for this board's
partition table — if a future release changes it, or a release for a
different board uses a different value, that'll be called out in the
release notes alongside the files.

**Later updates over WiFi.** Once the board is set up and on your network,
there's no need for USB again: open its web UI, go to the **Update** page,
choose `esp32-s3-devkitc1-n16r8-ota-update.bin`, and flash. The board
installs the new firmware and web UI, keeps its saved settings, and
restarts. (Or from a script, see `POST /api/ota/upload` under [API](#api).)

## API

Everything the web UI does goes through a plain HTTP API on port 80, so
anything below can be scripted directly — reach it at
`http://<hostname>.local` (mDNS) or the board's IP, or `http://192.168.4.1`
while it's hosting its own fallback AP. Every JSON endpoint responds
`{"ok": true, ...}` on success or `{"ok": false, "error": "..."}` on
failure (HTTP 400 for a bad request, 503 for RT4K-side failures).
POST bodies are `application/x-www-form-urlencoded` unless noted.

**Status & serial**
- `GET /api/status` → `{"ftdi_connected", "baud", "wifi_ssid", "wifi_ip",
  "ota_supported"}`
- `POST /api/serial` — `baud` (`2000000` or `115200`) — switches the
  ESP32↔FT232R link speed to match the RT4K firmware's expected baud.
- `POST /api/command` — `command`, a raw line sent verbatim to the RT4K's
  console (same as typing into the Terminal view).

**Remote control**
- `POST /api/remote` — `button`, one of the RetroTINK remote button names:
  `pwr menu up down left right ok back diag stat input output scaler sfx
  adc col aud prof prof1-12 gain phase pause safe genlock buffer res4k
  res1080p res1440p res480p res1-4 aux1-8`. Queued and sent to the RT4K in
  arrival order.

**WiFi**
- `GET /api/wifi` → `{"mode": "station"|"access point", "ssid", "ip",
  "hostname", "scan"}` (`scan` is the last cached scan, same shape as
  `/api/wifi/scan`'s response)
- `POST /api/wifi` — `ssid`, `password` (optional) — saves credentials and
  reboots into station mode
- `POST /api/wifi/forget` — clears saved credentials and reboots into AP
  mode
- `POST /api/wifi/hostname` — `hostname` (letters/digits/hyphens only,
  1-32 chars, can't start or end with a hyphen) — saves and reboots
- `GET /api/wifi/scan` → `{"ok", "networks": [{"ssid", "rssi", "secure"},
  ...]}`, sorted by signal strength — triggers a live scan (takes a few
  seconds)

**Profiles** (paths are relative to `/profile` on the RT4K's SD card)
- `GET /api/profiles/current` → `{"ok", "loaded", "path"}` (`path` only
  present when `loaded`)
- `GET /api/profiles?path=<subpath>` → `{"ok", "entries": [{"name",
  "isDirectory", "size"}, ...]}` (`path` defaults to the profile root)
- `POST /api/profiles/load` — `name` — resets the RT4K's live settings to
  whatever's stored in that profile
- `POST /api/profiles/delete` — `name`
- `GET /api/profiles/download?name=<name>` → the raw `.rt4` file
  (`Content-Disposition: attachment`)
- `POST /api/profiles/upload?name=<name>` — raw binary body (the `.rt4`
  file itself, up to 8MB) with `Content-Type: application/octet-stream`, e.g.
  `curl -H "Content-Type: application/octet-stream" --data-binary @local.rt4 "http://<host>/api/profiles/upload?name=my.rt4"`

**RT4K firmware** — stage RetroTINK firmware files at the root of the SD
card and install them, without pulling the card or using the RT4K's own menu
(Advanced Settings > OSD/Firmware > Check SD Card still works too)
- `POST /api/firmware/upload?name=<name>` — raw binary body (up to 8MB), same
  `Content-Type: application/octet-stream` requirement and rejection as OTA
  below, e.g.
  `curl -H "Content-Type: application/octet-stream" --data-binary @rt4kup.bin "http://<host>/api/firmware/upload?name=rt4kup.bin"`.
  `name` must be a bare filename (no `/`) since the RT4K looks for its update
  files at the card's root, not a subfolder. A ~4.6MB `.rbf` takes about a
  minute: roughly 20s to reach the ESP32 over WiFi, then ~38s streaming to
  the RT4K under RTS/CTS flow control (measured on RT4K firmware 1.80.1).
- `GET /api/firmware/check` → `{"ok", "version", "token"}` — has the RT4K
  validate the `rt4kup.bin` currently on the card (`fwup check`) and report
  the version it would install. That file decides the version, not the
  `.rbf` files, so upload the new one along with them.
- `POST /api/firmware/install` — `token` (from `/api/firmware/check`) —
  starts flashing (`fwup go`); the RT4K reboots for about 40 seconds.
- `GET /api/firmware/device` → `{"ok", "modelId", "model", "version"}` — the
  connected RT4K's model (`model`, e.g. `RT4K_Pro`) and running firmware
  (`ver`). The web UI uses it to pick the matching `.rbf` from a firmware
  zip (`rt4k_*`, `rt4kce_*` or `rt6x_*`).

**OTA update** (only present on boards built with `OTA_ENABLED` — see
[Adding a new board](#adding-a-new-board); `/api/status`'s `ota_supported`
field says whether this build has it)
- `POST /api/ota/upload` — the single combined file `pio run -t release`
  packages as `<env>-ota-update.bin` (firmware and the web UI's filesystem
  image together — see that section for what's in it), e.g.
  ```
  curl -H "Content-Type: application/octet-stream" \
    --data-binary @esp32-s3-devkitc1-n16r8-ota-update.bin http://<host>/api/ota/upload
  ```
  The `Content-Type` header is required, not optional — without it curl
  defaults to `application/x-www-form-urlencoded` for `--data-binary`, which
  makes ESPAsyncWebServer try to buffer the whole multi-MB body byte-by-byte
  into a String for form parsing instead of streaming it, crashing the device
  partway through. The endpoint rejects any other wrong Content-Type with a
  clean 400 -- this is the one case it can't, since the crash happens inside
  ESPAsyncWebServer before the request ever reaches this handler.
  Applies immediately and reboots on success; there's no confirmation step
  and no rollback, so double-check it's the right file for the board
  before sending it. The filesystem is written first and the firmware
  second, so a bad or interrupted upload only ever risks the web UI (fixed
  by re-uploading a good file — every other endpoint here, including this
  one, doesn't depend on it and stays reachable either way) and never
  leaves the board with broken firmware.

**WebSockets**

Three WebSocket endpoints back the UI and are usable directly too:
- `/ws` — the live terminal relay. Send a line to run it on the RT4K's
  console; every line it prints back comes through the same socket. The
  simplest way to script arbitrary commands interactively.
- `/ws/osd` — a small JSON request/response protocol for OSD snapshot
  data (font/plane/banner), used by the OSD view. It also pushes a
  `remote` event (`button`, `ok`, no `id`) once the queued remote presses
  have all been sent; the OSD view refreshes on it. See
  [osd_endpoints.cpp](src/web/osd_endpoints.cpp) for the message format if
  you need it; not intended as a general integration point.
- `/ws/firmware` — one-way JSON status events during firmware uploads and
  installs: `progress` (`file`, `sent`, `total` — bytes written to the
  RT4K), `verifying`, `uploaded`, `checking`, `checked` (`version`),
  `installing`, `flashing`, and `error` (`message`).

Everything below this point is for building from source instead of using a
release — not needed just to flash or update a board.


## Building the firmware

Requires [PlatformIO](https://platformio.org/) (CLI or the VS Code
extension).

```sh
pio run                       # build
pio run -t upload             # build + flash over the UART port
pio device monitor            # serial log output
```

## Building and flashing the web UI

The web UI is flashed as a separate LittleFS image. Install its
dependencies once:

```sh
cd web && npm install && cd ..
```

Then:

```sh
pio run -t uploadfs           # builds web/ into data/ and flashes it
```

Run it after a fresh flash and whenever `web/` changes; firmware and web UI
flash independently (`upload` vs. `uploadfs`). `buildfs`, `uploadfs`,
`release` and `ota` all run `npm run build` (svelte-check, then Vite) first.

## Adding a new board

Board configs live in [platformio.ini](platformio.ini) as `[env:<name>]`
sections. To add one:

1. Confirm the target chip has native USB-OTG host support (S2/S3/P4 —
   see Hardware above). This is a hardware requirement of `EspUsbHost`,
   not something the firmware can work around.
2. Add a new env block, e.g.:
   ```ini
   [env:my-new-board]
   platform = https://github.com/pioarduino/platform-espressif32/releases/download/stable/platform-espressif32.zip
   framework = arduino
   board = <platformio board id>
   board_build.filesystem = littlefs
   ```
   Find the board id with `pio boards espressif32` (or search
   [PlatformIO's board list](https://docs.platformio.org/en/latest/boards/index.html#espressif-32)).
3. Build with `pio run -e my-new-board` / flash with
   `pio run -e my-new-board -t upload`.
4. Re-check wiring: whichever USB port maps to the chip's native USB
   peripheral (not the UART-bridge port) is the one that connects to the
   RT4K.
5. `pio run -t release` works with any partition table that has a
   `littlefs`/`spiffs` data partition. [scripts/release.py](scripts/release.py)
   takes every offset from PlatformIO, so it needs no per-board changes.
   The board's default table is used unless the env sets
   `board_build.partitions = <file>.csv`.
6. To offer OTA updates (`/api/ota/upload` and the web UI's Update page)
   on this board, add `build_flags = -D OTA_ENABLED=1` to its env. This
   needs a partition table with two "app" slots (`ota_0`/`ota_1`) plus an
   "otadata" partition. Most boards' default tables already have them.

No firmware source changes are needed for a same-family (S2/S3/P4) board
swap — everything board-specific is confined to `platformio.ini`.

## Building a release image

Normal development flashes the program and filesystem separately
(`upload` / `uploadfs`). For distributing a release,
[scripts/release.py](scripts/release.py) registers a `release` PlatformIO
target that builds everything and merges it into flashable images with one
command:

```sh
pio run -t release            # also builds the web UI
```

This shows up as a normal task in the PlatformIO IDE too (VS Code's
PlatformIO extension just lists whatever targets exist for an
environment), not just on the CLI.

It produces up to three files in `.pio/build/<env>/`:

| File | Contains | Written at | Use it for |
|---|---|---|---|
| `<env>-blank-board-init.bin` | everything: bootloader + partition table + `otadata` + firmware + web UI filesystem | `0x0` via esptool | A factory-blank board, once (resets saved settings) |
| `<env>-update.bin` | `otadata` + firmware + web UI filesystem | `0xe000` via esptool | Updating over USB |
| `<env>-ota-update.bin` | firmware + web UI filesystem | nowhere — uploaded over WiFi | Updating over WiFi (`OTA_ENABLED` builds only) |

- **`<env>-blank-board-init.bin`** — the complete flash image, from the
  second-stage bootloader at `0x0` through the web UI filesystem: bootloader,
  partition table, then the same contents as `update.bin`. A factory-blank
  chip has no bootloader or partition table, so this one file is all it
  needs. Its padding covers `nvs` with `0xFF` (erased flash, which the
  firmware treats as no saved settings), so flashing it onto a board that's
  already set up resets its WiFi/hostname/baud — use `update.bin` for that.
- **`<env>-update.bin`** — **the file to distribute** for USB updates. It
  starts at `otadata` (so the board boots the first app slot), then the
  firmware in `app0`, then the web UI's LittleFS image in its filesystem
  partition, with `0xFF` padding over the gaps between them — which is why
  it's around 16MB. It stops before the `coredump` partition and starts
  after `nvs`, so a board's saved WiFi/hostname/baud survive.
- **`<env>-ota-update.bin`** — the same firmware and web UI, for
  `POST /api/ota/upload` (the web UI's Update page) instead of `esptool`.
  It isn't a flash image: it's a 13-byte header (`RT4O`, a version byte,
  then the filesystem and firmware lengths as little-endian uint32s)
  followed by the filesystem image and the firmware. The device writes the
  filesystem first and the firmware second, into the app slot it isn't
  running from, then reboots into it. Only built when the env sets
  `OTA_ENABLED` (see [Adding a new board](#adding-a-new-board)); see
  [API](#api) for the endpoint.

`pio run -t ota` builds that package and uploads it to the device set by
`custom_ota_host` in [platformio.ini](platformio.ini) (`tinkesp32.local`).
Set `OTA_HOST=<host>` to target a different device.


**Why two USB images.** An image written from `0x0` pads straight across
the `nvs` partition with `0xFF` and erases it. That's fine for a blank
board, which has no settings yet, but would reset a running board's saved
settings on every update. So the blank-board image covers the whole flash,
and the update image starts after `nvs` and never overlaps it.

The other files in that folder aren't for distribution: `firmware.bin`,
`bootloader.bin`, `partitions.bin` and `littlefs.bin` are the pieces the
images above are merged from; `firmware.elf` and `firmware.map` keep debug
symbols and the linker layout (the monitor's exception decoder uses the
`.elf`); and `firmware.factory.bin` is PlatformIO's own merged image, which
leaves out the web UI.

Every offset comes from PlatformIO's own build of the env (bootloader,
partition table, app, and the filesystem partition), and the images are
merged with `esptool merge-bin`.

The script prints ready-to-copy `esptool` commands for both cases:

```sh
# first flash of a blank board
esptool --chip esp32s3 --port <PORT> write-flash \
  0x0    esp32-s3-devkitc1-n16r8-blank-board-init.bin

# updating a board that's already running
esptool --chip esp32s3 --port <PORT> write-flash \
  0xe000 esp32-s3-devkitc1-n16r8-update.bin
```

(`0xe000` is where `otadata` starts on this board; the script prints the
offsets for your build.) Install esptool with `pip install esptool` if it
isn't on your `PATH`.
