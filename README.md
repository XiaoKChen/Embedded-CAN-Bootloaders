# Arduino UNO R4 Minima — CAN + DFU Bootloader (Embassy rewrite)

A custom bootloader for the Arduino UNO R4 Minima (Renesas **RA4M1**, Cortex-M4F) that adds
**flash-over-CAN** and flash-time-configurable USB DFU identity/CAN-IDs on top of the stock
upload workflow. This branch (`version/embassy`) is a from-scratch rewrite of the bootloader in
Rust on the [Embassy](https://github.com/embassy-rs/embassy) async framework, running directly
against the `ra4m1` PAC (no Embassy HAL exists for RA yet). The original C bootloader (TinyUSB +
FSP) lives on `main` and is preserved as both prior art and the recovery/reference image.

Full design record: [`docs/plan.md`](docs/plan.md) (locked decisions, workspace layout, milestone
plan) and [`docs/research/`](docs/research/) (behavior spec + ecosystem findings the plan is
built on).

## Status

| Milestone | State | Commit |
|---|---|---|
| **M0** — scaffolding & risk retirement | **Done** | [`6be6739`](../../commit/6be6739) |
| **M1** — CAN bootloader at parity (minus USB) | **Done** | [`2b0976a`](../../commit/2b0976a) |
| **M2** — USB DFU | Upcoming | — |

**M0** stood up the Cargo workspace, retired the hardware-bring-up risks as spikes (clocks, OFS
option bytes, VBTBKR handoff, executor + a custom GPT320 `embassy-time` driver — see
[`docs/bench/m0-spikes.md`](docs/bench/m0-spikes.md)), and landed the pure `boot-logic` crate
(memory map, CRC32, image metadata, CAN Update Protocol v1 engine, DFU config parsing) with 88
host tests.

**M1** brought the CAN bootloader to feature parity with the C original, USB excepted: a
raw-MMIO flash-LP driver (RAM-resident program/erase, data-flash read gate), a polled-FIFO CAN0
driver, the boot decision + app-jump sequence, the `can_update` protocol task, the parked-mode LED
fade, a demo app linked at the new app-slot base (`0x8000`), and a hardened CI (fmt, clippy, host
tests, size gate, OFS byte-check, release artifacts).

Software gates are all green, and M1 passed its first hardware smoke test (2026-07-17):
flashed via an ESP-Prog-2 probe running custom `esp-usb-bridge` firmware (CMSIS-DAP over SWD) —
`probe-rs run` builds, flashes, and streams `defmt`-over-RTT logs in one command. On target: HOCO
48 MHz clock tree up (ICLK 48 / PCLKA 48 / PCLKB 24 / FCLK 24), boot decision parks fail-closed
with no valid app present, the GPT ch3 PWM LED fade runs, the USB-transceiver rail powers up, the
data-flash read gate passes, the DFU config block reads clean from data flash (provisioned CAN
IDs honored), and CAN0 serves at 250 kbit/s with a 500 µs RX poll ticker draining the FIFO.
`bootloader.bin` is 16,964 B of the 32,768 B size-gate budget, and the OFS0/OFS1/Security-MPU
option bytes are byte-identical to the stock dump. All 88 host tests pass.

Remaining M1 exit criteria — full end-to-end `can_flash.py` flashing over CAN, double-tap
re-entry, and a power-loss spot check — need a CAN host adapter on the bench and are tracked in
[`docs/bench/m0-spikes.md`](docs/bench/m0-spikes.md) / `docs/plan.md` §6.

**M2** (USB DFU device mode, sharing the flash driver behind a mutex with the CAN task in parked
mode) is planned per `docs/plan.md` §6 but not started.

## Memory map

Locked in `docs/plan.md` §1 (supersedes the C-era 16 KB/238 KB split) and enforced by
`boot-logic::memory_map`'s const-asserts:

| Region | Address range | Size | 2 KB blocks |
|---|---|---|---|
| Bootloader | `0x0000_0000`–`0x0000_7FFF` | 32 KB (`BOOT_SIZE`) | 16 |
| Application slot | `0x0000_8000`–`0x0003_F7FF` | 227,328 B (`APP_SLOT_SIZE`) | 111 |
| Image metadata block | `0x0003_F800`–`0x0003_FFFF` | 2 KB (`METADATA_ADDR`) | 1 |

OFS0/OFS1 option bytes and the Security-MPU words sit at flash `0x400`–`0x43F` inside the
bootloader region, byte-identical to stock. Every application (the demo app included) now links
at `0x8000`, not the stock `0x4000` — an accepted, flagged consequence of the region growing to
fit the Embassy executor + defmt (`docs/plan.md` §1, item 2).

## Repo layout

```
crates/
  boot-logic/       pure logic (no_std, zero HAL/PAC/executor deps, host-testable):
                     memory map, CRC32, image metadata, CAN Update Protocol v1 engine,
                     DFU config parsing
  ra4m1-embassy/     shared chip runtime: clock bring-up, custom embassy-time driver
                     (GPT320), VBTBKR backup-register access, PFS pin-function writes
bootloader/          the bootloader binary crate: boot decision, drivers (flash-LP,
                     CAN0, USB rail), tasks (can_update, led)
demo-app/            Embassy blinky linked at 0x8000, used to exercise the update path
docs/
  plan.md            implementation plan — locked decisions, workspace layout, milestones
  research/          behavior spec (master-feature-spec.md) + ecosystem findings
  bench/             on-hardware bench checklists (m0-spikes.md)
```

## Build & flash

Toolchain is pinned via [`rust-toolchain.toml`](rust-toolchain.toml) (stable, with the
`thumbv7em-none-eabihf` target and `llvm-tools`); `rustup show` picks it up automatically.

```sh
# Build the firmware
cargo build --release

# Host tests for the pure-logic crate (target triple can't run the test harness — see
# .cargo/config.toml for why the explicit --target is required)
cargo test -p boot-logic --target "$(rustc -vV | sed -n 's/host: //p')"

# Build, flash, and stream defmt-RTT logs in one command (probe-rs runner, chip R7FA4M1AB)
cargo run --release -p bootloader
```

Any CMSIS-DAP/SWD probe works with the `probe-rs` runner above. The M1 hardware smoke test used
an ESP-Prog-2 running custom `esp-usb-bridge` firmware (CMSIS-DAP over SWD) as a low-cost probe.

## CI

`.github/workflows/ci.yml` runs on every push to `main`/`version/**` and every PR:

- `cargo fmt --all -- --check`
- `cargo clippy -- -D warnings` (firmware members on the target triple, `boot-logic` on the host)
- `cargo test -p boot-logic` (host)
- `cargo build --release` for `thumbv7em-none-eabihf`, then a hard **32,768 B** size gate on
  `bootloader.bin` and a byte-check of the OFS0/OFS1/Security-MPU option bytes against the stock
  dump (brick-risk region — any mismatch fails the build)
- Build artifacts (`.bin`/`.elf`/`.hex`) are uploaded on every run

Pushing a `v*` tag additionally publishes a GitHub Release with those artifacts and
auto-generated notes.

## Further reading

- [`docs/plan.md`](docs/plan.md) — the implementation plan: locked architectural decisions,
  stack choices, workspace layout, concurrency architecture, and the milestone/unit breakdown.
- [`docs/research/master-feature-spec.md`](docs/research/master-feature-spec.md) — the
  implementation-independent behavior spec (what the C bootloader does, and every hardware
  constant it relies on).
- [`docs/research/embassy-ra4m1-ecosystem.md`](docs/research/embassy-ra4m1-ecosystem.md) —
  ecosystem findings (PAC choice, prior art, Embassy crate versions).
- [`docs/bench/m0-spikes.md`](docs/bench/m0-spikes.md) — on-hardware bench checklist for the M0
  bring-up spikes.
