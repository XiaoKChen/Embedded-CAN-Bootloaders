# Embedded CAN Bootloaders

Custom bootloaders that let a microcontroller be reflashed **over a CAN bus**, one
implementation per MCU target and language. Every implementation speaks the same
wire protocol — **CAN Update Protocol v1** — so a single host tool can flash, identify,
scan, and reboot any of them, and boards of different families can share one bus.

This `master` branch holds documentation only. **The code lives on the `version/*`
branches below** — check out the one for your target.

## Branches

Branch names follow `version/<language>/<target>`.

| Branch | Language / framework | Target | Status |
|---|---|---|---|
| [`version/c-cpp/ra4m1`](../../tree/version/c-cpp/ra4m1) | C11 (TinyUSB + Renesas FSP, GNU Make) | Arduino UNO R4 Minima — Renesas RA4M1, Cortex-M4F | Stable — CI builds + tagged releases |
| [`version/embassy/ra4m1`](../../tree/version/embassy/ra4m1) | Rust, [Embassy](https://github.com/embassy-rs/embassy) async | Arduino UNO R4 Minima — Renesas RA4M1, Cortex-M4F | Rewrite at parity (M0–M3 done) |
| [`version/c-cpp/esp32`](../../tree/version/c-cpp/esp32) | C (ESP-IDF, CMake) | Custom ESP32-S3 boards: brain (ESP32-S3-WROOM-1U-N16R8) + CAN nodes (ESP32-S3FH4R2) | Hardware-verified (phases 0–6); Wi-Fi/BLE pending |

```sh
git clone https://github.com/XiaoKChen/Embedded-CAN-Bootloaders.git
cd Embedded-CAN-Bootloaders
git switch version/c-cpp/ra4m1      # or version/embassy/ra4m1, version/c-cpp/esp32
```

## What each target does

### RA4M1 — `version/c-cpp/ra4m1` and `version/embassy/ra4m1`

A fork of Arduino's stock USB-DFU bootloader for the UNO R4 Minima that keeps the
stock DFU upload workflow, USB PID, and memory map, and adds:

1. **Flash-over-CAN** on D4 (CTX0) / D5 (CRX0) via a polled CAN0 driver.
2. **Flash-time-configurable USB DFU names**, read from a data-flash config block.
3. **Per-device CAN update IDs**, so several boards can sit in bootloader mode on
   one bus and be addressed individually (default `0x79E` host→boot / `0x79F` boot→host).

The bootloader owns flash base `0x0000`; the application sits in a single slot
above it and is CRC32-validated before every jump. The C branch is the reference
and recovery image; the Embassy branch is a from-scratch Rust rewrite with the same
protocol, behavior, and host tests.

### ESP32-S3 — `version/c-cpp/esp32`

The ESP32's ROM bootloader can't be replaced and ESP-IDF owns the second stage, so
the CAN updater is built around the chip's partition table instead of a custom
stage-0 bootloader. The branch targets a two-board system: a **brain** reached
over Ethernet that updates one or more **nodes** over CAN (250 kbit/s, TWAI).

- **Node CAN updater** — an app in the node's `factory` partition speaks
  Protocol v1 over TWAI and writes the received image into the inactive
  `ota_0` / `ota_1` slot, with ESP-IDF app rollback enabled.
- **Node recovery without buttons** — a second-stage bootloader override falls
  back to the updater after 3 crashes in a row (RTC counter), or after 3 quick
  resets on its reset line (flash counter), which the brain can trigger.
- **Brain** — Ethernet HTTP (W5500) with its own A/B OTA and self-test rollback;
  it scans for, recovers and flashes nodes over CAN from a C port of the host
  tool, and can act as a USB-SLCAN adapter for the PC.
- **Update packages** — one file built by `tools/pkg` carries a brain image and
  up to 8 node images; the brain updates itself first, then relays each node
  image, waking running nodes with a CAN `ENTER_UPDATER` command.
- **Reference node app** (`examples/node_app`) — answers `APP_INFO` /
  `ENTER_UPDATER`, marks itself valid after a self-test.
- **One-command build** — `build.ps1` / `build.sh` install a pinned ESP-IDF into
  the repo and build everything from a fresh clone.

## Shared pieces

- **CAN Update Protocol v1** — HELLO / ERASE / SIZE / DATA / VERIFY / GO / INFO
  commands (the ESP32 branch adds APP_INFO / ENTER_UPDATER for running apps), 6 data bytes per frame, CRC32-verified. Specified and host-tested in
  each branch (`src/boot/can_update.{c,h}` in the C branches, the `boot-logic`
  crate in the Embassy branch).
- **`can-flash`** — the python-can / Textual host tool (`tools/can-flash/` on the C
  branches; it drives the Embassy bootloader unchanged) that flashes an application
  `.bin`, and can `--info`, `--scan` the bus, and `--enter-bootloader` remotely.

## Releases

Prebuilt binaries are published on the [Releases](../../releases) page by each
branch's CI workflow. Release tags carry the branch's `<language>-<target>` prefix
so the variants never collide:

| Branch | Release tag |
|---|---|
| `version/c-cpp/ra4m1` | `c-cpp-ra4m1-vX.Y.Z` |
| `version/embassy/ra4m1` | `embassy-ra4m1-vX.Y.Z` |
| `version/c-cpp/esp32` | `c-cpp-esp32-vX.Y.Z` |

Releases made before this scheme use plain tags: `v1.0.0`–`v1.0.2` are the C/C++
RA4M1 bootloader and `v0.1.0-alpha.1` is the Embassy RA4M1 bootloader.
