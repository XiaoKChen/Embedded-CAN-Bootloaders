# ESP32-S3 CAN Updater

Firmware and tools for a two-board ESP32-S3 system. The **brain**
(ESP32-S3-WROOM-1U-N16R8 + W5500 Ethernet) updates itself over HTTP and
updates **nodes** (ESP32-S3FH4R2) over classic CAN (TWAI). Each node runs a
factory **CAN updater** that takes application images over CAN Update
Protocol v1 — the same protocol and host tool as the UNO R4 Minima (RA4M1)
bootloader on `version/c-cpp/ra4m1`.

```
PC ──Ethernet/HTTP──► brain ──CAN 250 kbit/s──► node ─► node ─► …
                        └──GPIO3 → node EN (recovery reset line)
```

## Features

- Node CAN updater in `factory`; apps go A/B into `ota_0`/`ota_1` with rollback.
- Brain self-update over Ethernet (`POST /ota`) with self-test and rollback.
- Brain flashes nodes over CAN (`/node/flash`, `/node/scan`); no PC CAN adapter.
- Update packages: one checksummed file, optional brain image + up to eight
  nodes, brain first, power-loss-resumable relay.
- Recovery without a pin: crash-loop and EN-pulse counters, CAN ENTER_UPDATER.
- Per-device CAN IDs and names, so many parked nodes share one bus.
- PC tools: `can_flash.py` (python-can flasher/scanner/TUI), `ecb_pkg.py`;
  the brain doubles as an SLCAN USB-CAN adapter.
- HTTP is trusted-LAN-only, without authentication or signing.

## Quick start

Build it (installs everything it needs on first run):

```sh
./build.sh                                              # Linux / macOS
powershell -ExecutionPolicy Bypass -File build.ps1      # Windows
```

Requires only Git and Python 3.8–3.13. The first run installs the pinned
ESP-IDF (`idf-version.txt`) into `.tools/`, then builds the node updater and
the brain. Details: [docs/getting-started/build.md](docs/getting-started/build.md).
Then flash both boards over USB once, **brain first**:
[docs/getting-started/first-flash.md](docs/getting-started/first-flash.md).

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
| [Configuration](docs/reference/configuration.md) | Kconfig, `sdkconfig.defaults`, build variants |
| [Updating nodes](docs/guides/updating-nodes.md) | package, `/node/flash`, `can_flash.py` |
| [Writing a node app](docs/guides/writing-a-node-app.md) | start from `examples/node_app` |
| [Provisioning CAN IDs](docs/guides/provisioning-can-ids.md) | per-device IDs and INFO name |
| [SLCAN bridge](docs/guides/slcan-bridge.md) | the brain as a USB-CAN adapter |
| [Host tests](docs/testing/host-tests.md) | Unity/CTest on the PC |
| [Bench tests](docs/testing/bench/README.md) | hardware procedures |
| [can-flash](tools/can-flash/README.md), [pkg](tools/pkg/README.md) | tool READMEs |

## Repository layout

```
.
├── build.sh / build.ps1   one-command build (repo-local ESP-IDF in .tools/)
├── idf-version.txt        the ESP-IDF tag every build uses (scripts + CI)
├── node_updater/          node CAN updater (factory app, can_updater.bin) + 4 MB table
├── brain/                 brain firmware (brain_ota.bin): Ethernet OTA, node flashing, 16 MB table
├── components/            shared ESP-IDF components (CAN host, TWAI, updater client, ...)
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

This is branch **`version/c-cpp/esp32`**. Other targets live on their own
branches (`version/c-cpp/ra4m1`, `version/embassy/ra4m1`). CI builds every
push; a `c-cpp-esp32-v*` tag publishes a GitHub Release
(`git tag c-cpp-esp32-v1.0.0 && git push origin c-cpp-esp32-v1.0.0`), see
[build.md](docs/getting-started/build.md#ci-and-releases).
