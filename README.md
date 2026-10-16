# ESP32-S3 CAN Updater

**Build it (installs everything it needs on first run):**

```sh
./build.sh                                              # Linux / macOS
powershell -ExecutionPolicy Bypass -File build.ps1      # Windows
```

Requires only **Git** and **Python 3.8–3.13**. The first run clones the pinned
ESP-IDF (`idf-version.txt`, currently v5.3.6) and its esp32s3 toolchain into
`.tools/` inside the repo (nothing goes to your user profile), then builds;
later runs only rebuild. With no argument it builds both boards' firmware,
the node's CAN updater (`node_updater/`) and the brain's (`brain/`), and
prints each binary and its flash offset. Checkout paths with spaces work on
Windows (via a data-less junction, see the guide); on Linux/macOS clone to a
path without spaces. Details: **[docs/build.md](docs/build.md)**.

A first argument names one project, and anything after it goes to `idf.py`:
`./build.sh node_updater`, `./build.sh brain -p /dev/ttyACM0 flash`, the
bench-test app `./build.sh examples/bench_app`, or the brain's USB-CAN
bridge `./build.sh examples/slcan_bridge` (on Windows: `build.ps1
node_updater -p COM64 flash`, `build.ps1 brain -p COM63 flash`). idf.py
arguments without a project are refused: they could mean either board. Testing a
node on the bench from a PC, with the brain (or any python-can adapter) as
the USB-CAN adapter: **[docs/bench-test.md](docs/bench-test.md)**. The brain
updating itself over Ethernet (`brain/`, issue #5): section 8 of the same
guide. **The brain can also flash parked nodes itself over CAN** (Phase 2,
issue #8): upload the node `.bin` over Ethernet to `/node/flash`, then poll
`/node/status`; `/node/scan` discovers command/reply IDs. No PC CAN adapter
is needed. See [section 9](docs/bench-test.md#9-brain-flashes-the-node-over-can-phase-2-issue-8).
**Phase 3 recovery (#9)** also catches confirmed-app crash loops and lets the
brain force the updater with `POST /node/recover`; see section 10 of that guide.
**Phase 5 packages (#10)** add `POST /package` and `GET /package/status`: one
checksummed file contains an optional brain image and up to eight node images.
The brain updates/confirms itself first, then relays nodes in order, with INFO
name checks, automatic GPIO3 recovery and a power-loss-resumable NVS record.
Build with the stdlib-only `tools/pkg/ecb_pkg.py`; install the new brain partition
table over USB once. See [section 11](docs/bench-test.md#11-update-package-phase-5-issue-10)
and the [package format](docs/package-format.md). HTTP remains trusted-LAN-only,
without authentication or signing.
**Phase 6 (#11)** adds **`examples/node_app`**, the reference app for real node
firmware: it answers **APP_INFO** and **ENTER_UPDATER** on the node's own update
IDs, so the brain's package relay (and `POST /node/flash`) parks a running node
over CAN first and pulses GPIO3 only when that fails; each node result says
which path was used. `GET /node/app_info` and `can_flash.py --app-info` /
`--enter-updater` identify or park a running app. See
[section 12](docs/bench-test.md#12-example-node-app-phase-6-issue-11).

## Repository layout

```
.
├── build.sh / build.ps1   one-command build (repo-local ESP-IDF in .tools/)
├── idf-version.txt        the ESP-IDF tag every build uses (scripts + CI)
├── node_updater/          ESP-IDF project: the node's CAN updater (factory app,
│                          can_updater.bin); its partitions.csv is the 4 MB
│                          layout every node OTA app shares
├── brain/                 ESP-IDF project: the brain updating itself over
│                          Ethernet and flashing nodes over CAN (brain_ota.bin), 16 MB table
├── components/            shared by the projects above and the examples
│   ├── boot_core/         compiles src/boot/ into the firmware
│   ├── can_flash_host/    pure C scan/flash host (callbacks, no heap, host-tested)
│   ├── can_link/          TWAI wrapper shared by updater, brain and bridge
│   ├── can_updater_client/  app-side updater entry + 10 s recovery health tracking
│   ├── node_reset/        the brain's GPIO3 reset line to the node
│   └── slcan_codec/       pure-logic SLCAN parser/encoder (host-tested)
├── examples/node_app/     reference node app: health, self-test, APP_INFO/ENTER_UPDATER
├── examples/bench_app/    minimal node OTA app for bench tests (docs/bench-test.md)
├── examples/slcan_bridge/ the brain as an SLCAN USB-CAN adapter (flashed over USB)
├── src/boot/              shared pure-logic core (protocol engine, CRC32,
│                          config/package parsers; image_validate/memory_map are
│                          RA4M1 prior art, host-tested but not in the firmware)
├── tests/                 Unity/CTest: boot core, SLCAN, CAN host vs real node engine
├── tools/can-flash/       python-can host flasher / scanner / TUI
├── tools/pkg/             stdlib-only package builder and integrity inspector
└── docs/                  build and bench-test guides; research/ holds the
                           RA4M1 prior art
```

---

Flash an application `.bin` onto custom **ESP32-S3** boards over classic CAN
(TWAI), using the same **CAN Update Protocol v1** — and the same host tool,
`tools/can-flash/can_flash.py` — as the UNO R4 Minima (RA4M1) CAN bootloader
on the `version/c-cpp/ra4m1` branch. Tracks issue #1.

The updater is an ordinary ESP-IDF app living in the **`factory`** partition,
booted by an ESP-IDF 2nd-stage override with recovery counters. It receives
an image over CAN, writes it to the inactive OTA slot with `esp_ota_*` (which validates the
ESP image format, chip ID and SHA-256), and on GO selects that slot and
restarts into it.

## Hardware

- **Boards:** ESP32-S3-WROOM-1U-N16R8 (16 MB flash, 8 MB octal PSRAM) and
  ESP32-S3FH4R2 (4 MB in-package flash, 2 MB PSRAM). One build serves both:
  the partition table fits 4 MB, and the updater does not use PSRAM.
- **CAN:** the ESP32-S3's TWAI controller via the GPIO matrix to an external
  **3.3 V transceiver** (SN65HVD230, TCAN332, …), 120 Ω at both bus ends.
  TX GPIO, RX GPIO and bitrate are menuconfig options
  (`./build.sh node_updater menuconfig` → *CAN Updater*); defaults **GPIO4 / GPIO5 /
  250 kbit/s**, matching the node board (ESP32-S3FH4R2 + TCAN332DR, issue
  #2). The brain (WROOM-1U-N16R8) uses GPIO6/GPIO7 at 250 kbit/s for its
  C CAN host, or can run the alternative USB-CAN bridge below. Its GPIO3
  node-reset output stays LOW except for `/node/recover` or a package's automatic
  recovery after no INFO answer (four 100 ms HIGH / 400 ms LOW pulses through
  the inverting stage). Only the physically wired node can be recovered.
- **No recovery pin:** GPIO4 is TWAI TX, so the bootloader's GPIO
  factory-reset is off (see below).

## SLCAN bridge (brain as USB-CAN adapter)

`examples/slcan_bridge` turns the brain into an **SLCAN (Lawicel) adapter**
on its native USB port, so `tools/can-flash` reaches a node's updater with no
other CAN hardware (issue #3):

```sh
powershell -ExecutionPolicy Bypass -File build.ps1 examples/slcan_bridge -p COM8 flash   # once, over USB
uv run tools/can-flash/can_flash.py app.bin --interface slcan --channel COM8 --bitrate 250000
```

It speaks exactly the SLCAN subset python-can uses (`C`/`O`/`L`, `S0`–`S8`
except 750 k, `t`/`T`/`r`/`R`, `V`, `N`), logs bus errors and frame loss to
UART0 (never to the SLCAN port), and drives GPIO3, the node's reset line
(HIGH = node held in reset), LOW so the node runs (`components/node_reset`,
shared with `brain/`). Setup and tests: [docs/bench-test.md](docs/bench-test.md).

## Flash layout (4 MB, `node_updater/partitions.csv`)

| Partition | Offset | Size | Contents |
|---|---|---|---|
| bootloader | `0x0` | — | ESP-IDF 2nd stage + recovery selection (rollback enabled) |
| partition table | `0x8000` | 4 KB | |
| `nvs` | `0x9000` | 20 KB | application data |
| `otadata` | `0xE000` | 8 KB | which OTA slot boots (erased = boot `factory`) |
| `phy_init` | `0x10000` | 4 KB | |
| `cancfg` | `0x11000` | 4 KB | per-device CAN-ID / name block (optional) |
| `bootcnt` | `0x12000` | 4 KB | recovery bit log (data subtype `0x42`); no existing offset moves |
| `factory` | `0x20000` | 384 KB | **this CAN updater** (~50 % free) |
| `ota_0` / `ota_1` | `0x80000` / `0x240000` | 1.75 MB each | the application |

## Brain partitions and package limits (16 MB)

`brain/partitions.csv` retains `nvs` at `0x9000`/`0x5000`, `otadata` at
`0xE000`/`0x2000`, `phy_init` at `0x10000`/`0x1000`, 3 MiB `ota_0` at `0x20000`,
3 MiB `ota_1` at `0x320000`, and 2 MiB `node_fw` at `0x620000`.
The new **`pkg`** partition (data subtype `0x43`) uses the formerly free range
`0x820000..0x1000000`, size **`0x7E0000` / 8,257,536 bytes**. Nothing moves.
The complete package must fit; per-image limits are 3,145,728 bytes for BRAIN and
1,835,008 bytes for each NODE. Eight maximum-size nodes plus a maximum-size brain
cannot fit. Packages do not update the bootloader or partition table.

All `/ota`, `/node/scan`, `/node/flash`, `/node/recover` and `/package` jobs share
one worker (409 when busy). Both `/node/status` and `/package/status` remain
responsive. A changed brain must reboot and confirm VALID before node relay;
rollback aborts relay. After a power cut mid-relay the NVS record restarts from
the first node, so earlier successes may be reflashed. Inspect individual node
results even when the package says `done`.

## How a board gets into the updater

| Path | Mechanism |
|---|---|
| **CAN request** (Phase 6, #11) | ENTER_UPDATER (`0x09`) on the node's own IDs, from the brain's relay, `/node/flash` or `can_flash.py --enter-updater`: an app with `examples/node_app`'s CAN service ACKs, then calls `can_updater_client_enter_updater()` |
| **App request** (`can_flash.py --enter-bootloader`) | the app calls `can_updater_client_enter_updater()` (`components/can_updater_client`), which selects `factory` and restarts — the ESP32 twin of the RA4M1 app writing the handoff magic and resetting |
| **No valid app** | fresh board, both slots invalid, or an app that was rolled back because it never called `esp_ota_mark_app_valid_cancel_rollback()` |
| **Software recovery** (Phase 3, #9) | three unclean SW/panic/WDT resets in RTC, or three power-on/EN boots in a flash bit log, force `factory` without changing `otadata`. Trips remain latched until 10 s of updater uptime or a verified GO. Normal `esp_restart()` is marked clean by the client shutdown handler |

The updater always parks waiting for the host. **App-requested** entry erases
`otadata` and remains parked across resets. **Counter-forced** entry preserves
`otadata`: after 10 s the counters clear but the updater keeps running; a
later reset may boot the previously selected app. A verified GO also clears
both counters before selecting the new image, so recovery can be followed
immediately by a flash; no 10 s wait is needed.

Apps must call `can_updater_client_start_health()` once early in `app_main`
and reserve the same 16 custom RTC bytes as `node_updater/sdkconfig.defaults`.
Rebuild older apps for clean-restart and stable-clear support. Existing nodes
need a one-time USB bootloader/table/updater flash; CAN cannot replace the
bootloader (bench guide section 10).

## Fail-safe

Nothing in the update path writes the `factory` partition. The only
boot-selection change is the `otadata` write on GO, which ESP-IDF makes atomic
across its two sectors. With app-requested factory entry, power loss before
GO comes back in the updater. With counter-forced entry, after stable clear
the prior OTA selection remains the fallback; re-trigger recovery if needed.
After GO the new app boots in `PENDING_VERIFY`; failure to confirm rolls back
to the previous valid image (or factory when none exists). Full table:
`node_updater/main/main.c`.

## CAN Update Protocol v1

Default IDs `0x79E` (host→board) / `0x79F` (board→host), or a per-device pair
from the `cancfg` block (same 80-byte v2 format the RA4M1 parses,
`src/boot/dfu_config.h`). 8-byte frames,
`HELLO → SIZE → ERASE → DATA windows → VERIFY → GO`, 2 KB ACK-paced windows,
CRC32 (IEEE/zlib) over the image, plus `INFO (0x07)` for discovery
(`--scan`). A running app (`examples/node_app`) answers `APP_INFO (0x08)` and
`ENTER_UPDATER (0x09)` instead; the updater NAKs those as unknown commands, so a
host tells app from updater (`src/boot/app_control.h`). The engine (`src/boot/can_update.c`) is shared, host-tested pure C
behind a slot HAL.

ESP32 deltas, all handled by `tools/can-flash` on this branch:

- HELLO announces a **1.75 MB** slot (896 windows). Window start seqs repeat
  every 128 windows, so can-flash now resolves a rewind NAK against the
  current/next window only (the wire format is unchanged).
- New NAK code **`0x06`** on VERIFY: the CRC matched, but `esp_ota_end`
  rejected the image (not an esp32s3 app, bad SHA-256).
- ERASE answers immediately; flash sectors are erased as DATA reaches them.

## Flashing an application over CAN

```sh
# Reboot a running app into the updater, then flash its OTA image
uv run tools/can-flash/can_flash.py path/to/app/build/my_app.bin \
    --interface socketcan --channel can0 --enter-bootloader

# Park a running node app (examples/node_app) over CAN, then flash
uv run tools/can-flash/can_flash.py path/to/app/build/my_app.bin \
    --interface socketcan --channel can0 --enter-updater

uv run tools/can-flash/can_flash.py --scan      # find parked boards
uv run tools/can-flash/can_flash.py             # interactive TUI
```

Integrating `can_updater_client` into an application, provisioning per-device
IDs, and the first USB/UART flash of a board: **[docs/build.md](docs/build.md)**.

## Tests and CI

```sh
cmake -S tests -B build-host && cmake --build build-host && ctest --test-dir build-host -C Debug --output-on-failure
```

`.github/workflows/esp32-build.yml` runs the host tests and `./build.sh`
(node updater + brain), then builds both examples, on every push / PR to
`version/c-cpp/esp32`. It uploads one artifact per board, each with its
bootloader, partition table, `otadata` image, app and `flash_args`:
`esp32s3-node-updater` (`can_updater.bin`) and `esp32s3-brain`
(`brain_ota.bin`). Pushing a **`c-cpp-esp32-v*`** tag publishes a GitHub
Release versioned by the tag minus its prefix (`c-cpp-esp32-v1.0.0` →
`v1.0.0`), with `esp32s3-node-updater-v1.0.0.zip`,
`esp32s3-brain-v1.0.0.zip` and the two app images. Every branch uses
`<lang>-<target>-v*` (`c-cpp-ra4m1-v*`, `embassy-ra4m1-v*`).

```sh
git tag c-cpp-esp32-v1.0.0 && git push origin c-cpp-esp32-v1.0.0
```
