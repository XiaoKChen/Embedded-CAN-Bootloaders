# ESP32-S3 CAN Updater

Firmware and tools for ESP32-S3 boards using one CAN updater and one
recovery bootloader. A per-board record in `cancfg` chooses TWAI pins,
bitrate and safety holds. The SOLO ODU (4 MB) is a node; the SOLO IDU
(16 MB) is both brain and node. The IDU can update itself over HTTP and
other nodes over classic CAN (TWAI). The factory updater takes application
images over CAN Update Protocol v1 — the same protocol and host tool as the
UNO R4 Minima (RA4M1) bootloader on `version/c-cpp/ra4m1`.

```
PC ──Ethernet/HTTP──► brain ──CAN 250 kbit/s──► node ─► node ─► …
                        └──GPIO3 → node EN (recovery reset line)
```

## Features

- One `bootloader.bin` and `can_updater.bin`, with `node-4MB` and
  `idu-16MB` layouts in the universal release bundle. Flash size patches the
  bootloader header at write time.
- Per-board `cancfg` profile selects TWAI TX/RX, bitrate and hold-LOW GPIOs;
  invalid/missing records fall back to TX4/RX5, 250 kbit/s, no holds.
- Provisioned updater TinyUSB CDC identity and 1200-baud ROM-download touch;
  UART0 remains the updater logging console.
- Brain self-update over Ethernet (`POST /ota`) with self-test and rollback.
- Brain flashes nodes over CAN (`/node/flash`, `/node/scan`); no PC CAN adapter.
- Update packages: one checksummed file, optional brain image + up to eight
  nodes, brain first, power-loss-resumable relay.
- Recovery without a pin: crash-loop and EN-pulse counters, CAN ENTER_UPDATER.
- Per-device CAN IDs and names, so many parked nodes share one bus; the IDU
  app also answers APP_INFO/ENTER_UPDATER on its own `0x07/0x08` pair.
- ESP-PROG-2-PY-CLI provisions both SOLO boards; opt-in PlatformIO Arduino
  node-app helper answers CAN requests and clears the flash boot counter.
- PC tools: `can_flash.py` (python-can flasher/scanner/TUI), `ecb_pkg.py`;
  the brain doubles as an SLCAN USB-CAN adapter.
- HTTP is trusted-LAN-only, without authentication or signing.

## Quick start

Build it (installs everything it needs on first run):

```sh
./build.sh                                              # Linux / macOS
powershell -ExecutionPolicy Bypass -File build.ps1      # Windows
```

Requires Git, Python 3.8–3.13 and network on first run. The script installs
the pinned ESP-IDF (`idf-version.txt`) into `.tools/` and fetches the
updater's `esp_tinyusb` 1.7.6 managed component, then builds the universal
updater bundles and separate `brain/` example. Details:
[build.md](docs/getting-started/build.md). Flash the IDU first, then ODU,
with ESP-PROG-2-PY-CLI and the universal bundle:
[first-flash.md](docs/getting-started/first-flash.md). The first planned
release for the bundle is `c-cpp-esp32-v1.2.0` (not yet tagged).

## Documentation

| Doc | Purpose |
|---|---|
| [docs/README.md](docs/README.md) | documentation index |
| [Build](docs/getting-started/build.md) | one-command build, outputs, Docker, CI and releases |
| [First flash](docs/getting-started/first-flash.md) | first USB flash of brain and node, ports, BOOT mode |
| [Wiring](docs/getting-started/wiring.md) | transceivers, pins, termination, GPIO3 reset line |
| [System overview](docs/concepts/system-overview.md) | how the pieces fit, update flows |
| [Flash layout](docs/concepts/flash-layout.md) | partition tables, package limits |
| [Recovery](docs/concepts/recovery.md) | how a node gets into its updater, fail-safe |
| [CAN protocol](docs/reference/can-protocol.md) | Protocol v1, APP_INFO / ENTER_UPDATER |
| [Brain HTTP API](docs/reference/brain-http-api.md) | every endpoint, status codes, JSON |
| [Package format](docs/reference/package-format.md) | `.ecbpkg` v1 specification |
| [Board profile](docs/reference/board-profile.md) | record layout, rules, defaults, SOLO golden vectors |
| [Provisioning](docs/guides/provisioning-can-ids.md) | recommended tool and manual v2/profile recipe |
| [PlatformIO Arduino](docs/guides/platformio-arduino.md) | stock-core node-app helper and RTC client path |
| [Configuration](docs/reference/configuration.md) | updater profile and remaining Kconfig/build variants |
| [Updating nodes](docs/guides/updating-nodes.md) | package, `/node/flash`, `can_flash.py` |
| [Writing a node app](docs/guides/writing-a-node-app.md) | start from `examples/node_app` |
| [SLCAN bridge](docs/guides/slcan-bridge.md) | the brain as a USB-CAN adapter |
| [Host tests](docs/testing/host-tests.md) | Unity/CTest on the PC |
| [Bench tests](docs/testing/bench/README.md) | hardware procedures |
| [can-flash](tools/can-flash/README.md), [pkg](tools/pkg/README.md) | tool READMEs |

## Repository layout

```
.
├── build.sh / build.ps1   one-command build (repo-local ESP-IDF in .tools/)
├── idf-version.txt        the ESP-IDF tag every build uses (scripts + CI)
├── node_updater/          common factory updater and recovery bootloader; 4 MB + 16 MB tables
├── brain/                 separate brain firmware example and its own 16 MB table
├── components/            shared ESP-IDF components and opt-in Arduino node-app helper
├── examples/node_app/     reference node app: health, self-test, APP_INFO/ENTER_UPDATER
├── examples/bench_app/    minimal node OTA app for bench tests
├── examples/slcan_bridge/ the brain as an SLCAN USB-CAN adapter (flashed over USB)
├── src/boot/              shared pure-logic core (protocol engine, CRC32, parsers)
├── tests/                 Unity/CTest: boot core, SLCAN, CAN host vs real node engine
├── tools/can-flash/       python-can host flasher / scanner / TUI
├── tools/pkg/             stdlib-only package builder and integrity inspector
└── docs/                  getting started, concepts, reference, guides, testing
```

## Branches and releases

The base ESP32 line is `version/c-cpp/esp32`; this issue #20 work is on
`feat/20-one-updater-board-profile`. Other targets have their own branches
(`version/c-cpp/ra4m1`, `version/embassy/ra4m1`). CI builds pushes; a
`c-cpp-esp32-v*` tag publishes a GitHub Release. The first planned tag with
the universal bundle is `c-cpp-esp32-v1.2.0` (not created yet). See
[build.md](docs/getting-started/build.md#ci-and-releases).
