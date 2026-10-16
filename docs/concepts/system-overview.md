# System overview

```
PC ──Ethernet (HTTP)──► brain ──CAN 250 kbit/s──► node(s)
                          │  └─GPIO3 → node EN (inverted reset line)
PC ──USB (SLCAN) ──► brain running the SLCAN bridge, or any python-can adapter ──CAN──► node(s)
```

## The pieces

| Piece | Where | Runs on | Role |
|---|---|---|---|
| Node CAN updater | `node_updater/` | node, `factory` partition | receives an app image over CAN (Protocol v1) into the inactive OTA slot |
| Recovery bootloader | `node_updater/bootloader_components/main` | node, 2nd stage | ESP-IDF 2nd stage + recovery counters ([recovery.md](recovery.md)) |
| Node apps | `examples/node_app` (reference), `examples/bench_app` (test) | node, `ota_0`/`ota_1` | the real firmware; delivered over CAN |
| Brain firmware | `brain/` (`brain_ota`) | brain | updates itself over HTTP; stages node images and flashes nodes over CAN; runs update packages |
| SLCAN bridge | `examples/slcan_bridge` | brain (alternative firmware) | turns the brain into a USB-CAN adapter for PC tools |
| Shared code | `components/`, `src/boot/` | firmware + host tests | TWAI wrapper, C CAN host, updater client, protocol engine, parsers |
| Host tools | `tools/can-flash`, `tools/pkg` | PC | python-can flasher/scanner/TUI; package builder/inspector |

The node updater flashes an application `.bin` onto custom **ESP32-S3**
boards over classic CAN (TWAI), using the same **CAN Update Protocol v1** —
and the same host tool, `tools/can-flash/can_flash.py` — as the UNO R4 Minima
(RA4M1) CAN bootloader on the `version/c-cpp/ra4m1` branch. Tracks issue #1.

The updater is an ordinary ESP-IDF app living in the **`factory`** partition,
booted by an ESP-IDF 2nd-stage override with recovery counters. It receives
an image over CAN, writes it to the inactive OTA slot with `esp_ota_*` (which
validates the ESP image format, chip ID and SHA-256), and on GO selects that
slot and restarts into it. The protocol engine (`src/boot/can_update.c`) is
shared, host-tested pure C behind a slot HAL. Wire format:
[can-protocol.md](../reference/can-protocol.md).

## The brain

`brain/` (firmware `brain_ota`) lets the **brain** update itself over its
**W5500** Ethernet port: an image POSTed over HTTP goes into the inactive OTA
slot, the brain reboots into it, and a self-test marks it valid or rolls it
back. Wi-Fi/BLE follow in issue #4: the OTA core (`brain/main/ota_update.c`)
takes image chunks from any transport, and the HTTP handler is just one
caller.

| Item | Value |
|---|---|
| Network | DHCP client; MAC = the chip's Ethernet MAC (base MAC + 3) |
| HTTP | port 80, [brain-http-api.md](../reference/brain-http-api.md) |
| Self-test | a fresh image (`PENDING_VERIFY`) must get an IP within **30 s**, then marks itself valid; otherwise it rolls back and reboots |
| Console | UART0 (GPIO43/44) and, as secondary output, USB-Serial-JTAG (the brain's USB port) |
| CAN | C CAN host (`components/can_flash_host`) over `can_link`, TX GPIO6 / RX GPIO7, 250 kbit/s |
| Node reset | GPIO3 → node EN, inverted ([wiring.md](../getting-started/wiring.md#brain-gpio3--node-en-reset-line)) |

**Trusted LAN only.** HTTP has no authentication or TLS, and packages are
unsigned: corruption detection, not authentication. **Only upload brain_ota
images to `/ota`.** Any valid ESP32-S3 app is accepted and booted. One that
confirms itself but has no Ethernet (the bench app, for instance) can then
only be replaced over USB.

## Update flows

1. **Brain updates itself** (issue #5): `POST /ota` with a raw `brain_ota.bin`
   → inactive slot → reboot → self-test → VALID, or rollback.
   Bench: [brain-ota.md](../testing/bench/brain-ota.md).
2. **Brain flashes one node** (issue #8): `POST /node/flash` stages the node
   `.bin` in the brain's `node_fw` partition, then the brain's C host runs
   ENTER_UPDATER → HELLO → SIZE → ERASE → DATA → VERIFY → GO over CAN. No PC
   CAN adapter is needed; `/node/scan` discovers command/reply IDs.
   Bench: [brain-can-flash.md](../testing/bench/brain-can-flash.md).
3. **Update package** (issue #10): one checksummed `.ecbpkg` with an optional
   brain image and up to eight node images (`tools/pkg/ecb_pkg.py`),
   `POST /package`. The brain updates and confirms itself first, then relays
   nodes in order, with INFO name checks, CAN or GPIO3 entry into each node's
   updater, and a power-loss-resumable NVS record.
   Format: [package-format.md](../reference/package-format.md);
   bench: [update-package.md](../testing/bench/update-package.md).
4. **PC flashes a node directly**: `tools/can-flash/can_flash.py` through the
   brain's SLCAN bridge or any python-can adapter.
   Guide: [updating-nodes.md](../guides/updating-nodes.md).

Each flow needs the node in its updater first. A running app built on
`examples/node_app` parks itself on ENTER_UPDATER over CAN (issue #11); the
brain falls back to GPIO3 recovery pulses when nothing answers. All paths:
[recovery.md](recovery.md).

## Shared components

```
├── components/            shared by the projects above and the examples
│   ├── boot_core/         compiles src/boot/ into the firmware
│   ├── can_flash_host/    pure C scan/flash host (callbacks, no heap, host-tested)
│   ├── can_link/          TWAI wrapper shared by updater, brain and bridge
│   ├── can_updater_client/  app-side updater entry + 10 s recovery health tracking
│   ├── node_reset/        the brain's GPIO3 reset line to the node
│   └── slcan_codec/       pure-logic SLCAN parser/encoder (host-tested)
```

`src/boot/` is the shared pure-logic core: protocol engine, CRC32,
config/package parsers; `image_validate`/`memory_map` are RA4M1 prior art,
host-tested but not in the firmware.
