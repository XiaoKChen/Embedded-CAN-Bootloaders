# Arduino UNO R4 Minima — CAN + DFU Bootloader

A fork of Arduino's stock TinyUSB **USB DFU** bootloader for the UNO R4 Minima
(Renesas **RA4M1**, Cortex-M4F), extended with three added features while keeping
the stock USB-DFU upload workflow, USB PID, and memory map unchanged:

1. **Flash-over-CAN** — program an application `.bin` over classic CAN 2.0B on
   **D4 (CTX0) / D5 (CRX0)**, using a small polled CAN0 driver and a windowed,
   CRC32-checked update protocol (**CAN Update Protocol v1**).
2. **Flash-time-configurable USB DFU names** — the DFU manufacturer/product
   strings are read at boot from a config block in data flash (written by the
   ESP-Prog-2 flasher), falling back to the compiled defaults when unset.
3. **Per-device CAN IDs** — each board's bootloader can be assigned its own CAN
   update ID pair (in that same config block), so multiple boards can sit in
   bootloader mode on one bus simultaneously and be addressed individually.
   Unprovisioned boards default to `0x79E` (host→boot) / `0x79F` (boot→host).
   Config reads are guarded fail-safe: if data flash isn't readable (e.g.
   after a software reset leaves the FACI in a bad state), the board still
   parks and serves CAN on the default IDs rather than faulting.

> Full project map: **[PROJECT.md](PROJECT.md)**. Build & flash guide:
> **[docs/build.md](docs/build.md)**. Design/decision record:
> **[docs/research/r4-minima-can-bootloader.md](docs/research/r4-minima-can-bootloader.md)**.

---

## Hardware / Peripherals

- **MCU / board:** Renesas RA4M1 (R7FA4M1AB) on the Arduino UNO R4 Minima, 5 V logic.
- **CAN:** the RA4M1's single classic **CAN0** channel — **D4 = P103 = CTX0**,
  **D5 = P102 = CRX0**. The board has **no on-board transceiver**: an external
  **5 V** CAN transceiver (TJA1050 / MCP2551 / MCP2562 / TCAN332) is required,
  with 120 Ω termination at both bus ends.
- **Transceiver power (custom boards):** CAN update mode switches on the
  RA4M1's internal **USB regulator (`USBMC.VDCEN`)** at entry, because custom
  boards may power the transceiver/level-shifter path from the USB `VUSB`
  rail. Without this, a board entering CAN mode via the app handoff (where
  USB is never initialized) sits deaf on the bus — operate mode, TEC frozen
  at 0, no bus-idle ever seen. Harmless on boards that don't use that rail.
- **Bus:** classic CAN 2.0B, 11-bit IDs, **250 kbit/s** (HOCO-clocked; prefer
  250 k for clock-tolerance margin).

## CAN Update Protocol v1

Two fixed CAN IDs per device (default `0x79E` host→boot / `0x79F` boot→host, or
the per-device pair from the config block). 8-byte frames:

```
HELLO → SIZE → ERASE → DATA windows → VERIFY → GO
```

Each 2 KB flash block is one ACK-paced DATA window (342 frames); the image is
CRC32-validated (IEEE/zlib) and a metadata block is written last so power loss
never bricks the device. `INFO (0x07)` returns the board's product name so a
parked board can be identified over CAN. The pure-logic engine
(`src/boot/can_update.c`) is host-tested; `tools/can-flash/` is the host side.

## Build & Flash

Built by compiling the pure-logic `src/boot/` core into the vendored
`arduino-renesas-bootloader` fork (behind the `CAN_LOADER` and `DFU_CUSTOM_NAME`
Makefile flags). See **[docs/build.md](docs/build.md)** for the exact toolchain
(gcc 14.2.1 xPack, GNU Make + POSIX sh, the space-free-path `subst`), the size
gate, and the flashing/unbrick procedures (rfp-cli ROM boot mode, pyOCD/SWD).

```sh
# Firmware (from the vendored fork, via CMake wrapper or direct Make — see docs/build.md)
cmake --build build-host --target bootloader

# Host tests (pure-logic core: crc32, image_validate, can_update, dfu_config)
cmake -S tests -B build-host && cmake --build build-host && ctest --test-dir build-host --output-on-failure

# Flash the built bootloader to a board (SWD)
pyocd flash -e chip -a 0x0 -t r7fa4m1ab vendor/arduino-renesas-bootloader/_build/uno_r4/arduino-renesas-bootloader.hex
```

Language standards: **C11** (firmware), C++17 reserved for Arduino-framework
targets. CPU flags: `-mcpu=cortex-m4 -mfpu=fpv4-sp-d16 -mfloat-abi=hard -mthumb`.

## Flashing an application over CAN

Host tool: `tools/can-flash/can_flash.py` (uv-runnable; TUI on bare invocation,
CLI with flags). Requires an external 5 V transceiver on D4/D5.

```sh
# Interactive TUI
uv run tools/can-flash/can_flash.py

# Headless: reboot a running app into the bootloader, then flash
uv run tools/can-flash/can_flash.py firmware.bin --interface socketcan --channel can0 --enter-bootloader

# Discover all boards parked in the bootloader (per-device IDs)
uv run tools/can-flash/can_flash.py --scan
```

A device enters CAN update mode when the application writes the magic
`0xB007CAFE` to `VBTBKR[4]` and resets (or when no valid app is present). The
full no-touch cycle — `--enter-bootloader` → INFO discovery on the per-device
IDs → flash → GO back into the app — is bench-verified end-to-end. The
sibling **AIRFORM-PKG `airform-tui-can-flasher`** provides the same flow as a
pure-bash Debian package for ARM/Linux hosts.

## Releases & CI

Prebuilt bootloader binaries (`.hex`/`.bin`/`.elf`) are published on the
**[Releases](https://github.com/XiaoKChen/Arduino-Custom-Bootloader/releases)**
page — grab `bootloader-uno_r4-<version>.hex` and flash it over SWD (see above).

Builds are automated by GitHub Actions
(`.github/workflows/release.yml`, which calls `scripts/ci-build.sh`):

- **Every push / PR to `version/c-cpp/ra4m1`** builds the firmware on Linux with the pinned
  xPack **gcc 14.2.1** toolchain, fetches the `.gitignore`d CMSIS_5/FSP deps,
  and enforces the 16,384-byte size gate — the build artifacts are attached to
  the run.
- **Pushing a `c-cpp-ra4m1-v*` tag** additionally publishes a GitHub Release with the
  version-named `.hex`/`.bin`/`.elf` (the prefix is stripped, so tag
  `c-cpp-ra4m1-v1.0.3` yields `bootloader-uno_r4-v1.0.3.*`) and auto-generated
  notes. Releases up to `v1.0.2` used the old plain `v*` tags.

```sh
# Cut a release: tag and push — CI builds and publishes it.
git tag c-cpp-ra4m1-v1.0.3 && git push origin c-cpp-ra4m1-v1.0.3
```

The bootloader firmware has no embedded version field; "version" here is the
release tag and the artifact filenames.

## Repo Layout

```
.
├── src/boot/     pure-logic core (host-testable): memory_map, image/dfu-config
│                 validation, crc32, CAN Update Protocol v1 state machine
├── src/drivers/  polled register-level RA4M1 CAN0 driver (D4/D5)
├── tests/        Unity/CTest host tests for the pure-logic core
├── tools/        python-can host flasher (can-flash/)
├── docs/         build/flash guide, research doc, reference artifacts (stock hex, .ld)
├── scripts/      ci-build.sh — Linux dep-fetch + Make build used by CI
├── .github/      GitHub Actions build + tag-driven release workflow
├── vendor/       vendored third-party (arduino-renesas-bootloader fork, tinyusb, FSP, CMSIS_5)
└── CMakeLists.txt  root wrapper: `bootloader` target (wraps Make) + host tests
```

## Dependencies

Vendored under `vendor/` (no external package manager): the
`arduino-renesas-bootloader` fork (where `src/` is integrated), tinyusb 0.17.0,
Renesas FSP, ARM CMSIS_5, and standalone RA4M1 device headers. CMSIS_5 and FSP
are `.gitignore`d and fetched per `docs/build.md`. The host flasher depends on
`python-can`, `can-isotp`, and `textual` (see `tools/can-flash/requirements.txt`).
