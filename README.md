# ESP32-S3 CAN Updater

**Build it (installs everything it needs on first run):**

```sh
./build.sh                                              # Linux / macOS
powershell -ExecutionPolicy Bypass -File build.ps1      # Windows
```

Requires only **Git** and **Python 3.8–3.13**. The first run clones the pinned
ESP-IDF (`idf-version.txt`, currently v5.3.6) and its esp32s3 toolchain into
`.tools/` inside the repo (nothing goes to your user profile), then builds;
later runs only rebuild. The script ends by printing each binary and its flash
offset. Checkout paths with spaces work on Windows (via a data-less junction,
see the guide); on Linux/macOS clone to a path without spaces. Details:
**[docs/build.md](docs/build.md)**.

---

Flash an application `.bin` onto custom **ESP32-S3** boards over classic CAN
(TWAI), using the same **CAN Update Protocol v1** — and the same host tool,
`tools/can-flash/can_flash.py` — as the UNO R4 Minima (RA4M1) CAN bootloader
on the `version/c-cpp/ra4m1` branch. Tracks issue #1.

The updater is an ordinary ESP-IDF app living in the **`factory`** partition,
booted by the stock ESP-IDF 2nd-stage bootloader. It receives an image over
CAN, writes it to the inactive OTA slot with `esp_ota_*` (which validates the
ESP image format, chip ID and SHA-256), and on GO selects that slot and
restarts into it.

## Hardware

- **Boards:** ESP32-S3-WROOM-1U-N16R8 (16 MB flash, 8 MB octal PSRAM) and
  ESP32-S3FH4R2 (4 MB in-package flash, 2 MB PSRAM). One build serves both:
  the partition table fits 4 MB, and the updater does not use PSRAM.
- **CAN:** the ESP32-S3's TWAI controller via the GPIO matrix to an external
  **3.3 V transceiver** (SN65HVD230, TCAN332, …), 120 Ω at both bus ends.
  TX GPIO, RX GPIO and bitrate are menuconfig options
  (`./build.sh menuconfig` → *CAN Updater*); defaults **GPIO5 / GPIO6 /
  250 kbit/s**.
- **Recovery pin:** **GPIO4**, held low through reset for 5 s, forces the
  updater (see below).

## Flash layout (4 MB, `partitions.csv`)

| Partition | Offset | Size | Contents |
|---|---|---|---|
| bootloader | `0x0` | — | stock ESP-IDF 2nd stage (rollback + factory-reset GPIO enabled) |
| partition table | `0x8000` | 4 KB | |
| `nvs` | `0x9000` | 20 KB | application data |
| `otadata` | `0xE000` | 8 KB | which OTA slot boots (erased = boot `factory`) |
| `phy_init` | `0x10000` | 4 KB | |
| `cancfg` | `0x11000` | 4 KB | per-device CAN-ID / name block (optional) |
| `factory` | `0x20000` | 384 KB | **this CAN updater** (~50 % free) |
| `ota_0` / `ota_1` | `0x80000` / `0x240000` | 1.75 MB each | the application |

## How a board gets into the updater

| Path | Mechanism |
|---|---|
| **App request** (`can_flash.py --enter-bootloader`) | the app calls `can_updater_client_enter_updater()` (`components/can_updater_client`), which selects `factory` and restarts — the ESP32 twin of the RA4M1 app writing the handoff magic and resetting |
| **Recovery GPIO** | hold GPIO4 low through reset for 5 s: ESP-IDF's built-in `CONFIG_BOOTLOADER_FACTORY_RESET` erases `otadata` (no app data is erased) and the bootloader boots `factory` — no custom bootloader code |
| **No valid app** | fresh board, both slots invalid, or an app that was rolled back because it never called `esp_ota_mark_app_valid_cancel_rollback()` |

The updater always parks waiting for the host. Because selecting `factory`
erases `otadata`, a board stays in the updater across resets until an image is
flashed — it is never bricked, and always reachable over CAN.

## Fail-safe

Nothing in the update path writes the `factory` partition. The only
boot-selection change is the `otadata` write on GO, which ESP-IDF makes atomic
across its two sectors. Power loss before or during GO therefore comes back up
in the updater; after GO the new app boots in `PENDING_VERIFY` and rolls back
to the updater if it does not confirm itself. Full table: `main/main.c`.

## CAN Update Protocol v1

Default IDs `0x79E` (host→board) / `0x79F` (board→host), or a per-device pair
from the `cancfg` block (same 80-byte v2 format the RA4M1 parses,
`src/boot/dfu_config.h`). 8-byte frames,
`HELLO → SIZE → ERASE → DATA windows → VERIFY → GO`, 2 KB ACK-paced windows,
CRC32 (IEEE/zlib) over the image, plus `INFO (0x07)` for discovery
(`--scan`). The engine (`src/boot/can_update.c`) is shared, host-tested pure C
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

uv run tools/can-flash/can_flash.py --scan      # find parked boards
uv run tools/can-flash/can_flash.py             # interactive TUI
```

Integrating `can_updater_client` into an application, provisioning per-device
IDs, and the first USB/UART flash of a board: **[docs/build.md](docs/build.md)**.

## Tests and CI

```sh
cmake -S tests -B build-host && cmake --build build-host && ctest --test-dir build-host --output-on-failure
```

`.github/workflows/esp32-build.yml` runs the host tests and `./build.sh` on
every push / PR to `version/c-cpp/esp32` and uploads the bootloader, partition
table, `otadata` image and updater as artifacts. Pushing a
**`c-cpp-esp32-v*`** tag publishes a GitHub Release versioned by the tag minus
its prefix (`c-cpp-esp32-v1.0.0` → `v1.0.0`, asset
`esp32s3-can-updater-v1.0.0.zip`). Every branch uses `<lang>-<target>-v*`
(`c-cpp-ra4m1-v*`, `embassy-ra4m1-v*`).

```sh
git tag c-cpp-esp32-v1.0.0 && git push origin c-cpp-esp32-v1.0.0
```

## Repo layout

```
.
├── build.sh / build.ps1   one-command build (repo-local ESP-IDF in .tools/)
├── idf-version.txt        the ESP-IDF tag every build uses (scripts + CI)
├── CMakeLists.txt         ESP-IDF project; sdkconfig.defaults, partitions.csv
├── main/                  the updater app: TWAI link, slot HAL, CAN service
├── components/
│   ├── boot_core/         compiles src/boot/ into the firmware
│   └── can_updater_client/  app-side "enter the updater" call
├── src/boot/              shared pure-logic core (protocol engine, CRC32,
│                          config-block parser; image_validate/memory_map are
│                          RA4M1 prior art, host-tested but not in the firmware)
├── tests/                 Unity/CTest host tests for src/boot/
├── tools/can-flash/       python-can host flasher / scanner / TUI
└── docs/                  build guide; research/ holds the RA4M1 prior art
```
