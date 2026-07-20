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
| **M2** — USB DFU | **Done** | [`f9430fc`](../../commit/f9430fc) |
| **M2.5** — Renode emulation validation | **Done** — 12 cases, 4 suites green | pending commit |
| **M3** — LGPL-free application HAL | **Done** | [`57fdd8a`](../../commit/57fdd8a) |

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

**M2** built USB DFU device mode across three units: U2.1 ([`ec6f73c`](../../commit/ec6f73c))
implements the RA4M1 USBFS `embassy-usb-driver` from scratch (no prior art — even the community
`ra4-hal` left USB unimplemented); U2.2 ([`1beeaab`](../../commit/1beeaab)) is a hand-rolled DFU
1.1 class on `embassy-usb` (VID `0x2341`/PID `0x0369`, `@CodeFlash`/`@DataFlash` alt settings,
strings sourced from the provisioned config block); U2.3 ([`1b87646`](../../commit/1b87646))
combines `usb_dfu` + `can_update` + the LED-fade task in parked mode, sharing the flash driver
behind a `Mutex<NoopRawMutex>` (one erase/program block per acquire, never held across an await,
so USB and CAN both stay live between blocks). `bootloader.bin` is 31,564 B of the 32,768 B size
gate (1,204 B headroom) with the full USB stack linked; all 88 host tests, fmt, clippy, and the
OFS byte-check stay green.

M2 passed its first hardware bring-up (2026-07-17, IDU Articulation board via ESP-Prog-2 SWD +
defmt-RTT): the device enumerates on a real RA4M1 as a DFU device (`2341:0369`, Status OK) with
the provisioned product string, after six real bugs surfaced and were fixed
([`f9430fc`](../../commit/f9430fc)) — an `embassy-usb` `enable()`/`PowerDetected` deadlock, the
USB operational clock defaulting to an unpowered PLL instead of HOCO, the PHY transceiver needing
re-init once the clock was fixed, an ICU interrupt-clear-at-exit race that wedged delivery under
the USBFS combined IRQ's SOF rate, and SOF-based suspend/resume ported from TinyUSB. **Enumeration
and the device-descriptor control-read stage are hardware-confirmed; the full `dfu-util`
upload/download exchange is not yet bench-verified**, and — like the M1 CAN path — the end-to-end
`can_flash.py` run also still needs its CAN host adapter on the bench (see the M1 remaining exit
criteria above).

**M3** delivered the from-scratch, MIT/Apache-2.0 RA4M1 application HAL (`crates/ra4m1-hal`,
[`docs/plan-m3-hal.md`](docs/plan-m3-hal.md)) — for *applications* on this chip, not the
bootloader, replacing the LGPL-3.0 community `ra4-hal`/`ra4m1-ctpac`. It layers on the existing
`ra4m1-embassy` clocks/PFS/time-driver base, and the bootloader **never** depends on it (verified —
its 32 KB gate is untouched). A spike ([`docs/research/ra4m1-pac-coverage.md`](docs/research/ra4m1-pac-coverage.md))
confirmed every peripheral is fully typed in the `ra4m1` PAC, so all drivers are PAC-based (no raw
MMIO). Implemented: GPIO, a clock-query API for every domain, GPT timers/PWM (ch1–7; ch0 stays the
time driver), UART/SCI, SPI, I2C (RIIC), ADC14, and DAC12 — each exposing `embedded-hal` 1.0
blocking **and** `embedded-hal-async`/`embedded-io-async` traits, with a runnable example under
`crates/ra4m1-hal/examples/`. Async drivers wake via `embassy-sync` `AtomicWaker`. Chip-fact
BENCH-VERIFY items in the drivers (ICU event numbers, pin/PSEL mappings) are FSP/datasheet-derived
and still need silicon confirmation.

**M2.5** brought up the [Renode](https://github.com/renode/renode) emulator as a hardware-free test
harness ([`docs/plan-m2.5-renode.md`](docs/plan-m2.5-renode.md), [`renode/`](renode/)) and grew it
to **full per-peripheral coverage** — **12 cases across 4 Robot suites, all green on Renode
v1.16.1**. A custom GPT compare-match model (the one piece Renode's stock RA4M1 platform lacks) lets
the Embassy executor run off `embassy-time`; custom C# models then cover DAC12, ADC14, SPI, I2C/RIIC
(with a mock slave), FACI-LP flash program/erase, and CAN0 (frame-level via `CANHub`), while UART/SCI
rides Renode's stock model and VBTBKR is proven to survive a CPU reset. The suites are
`smoke`/`analog`/`serial`/`bootloader`; a negative control confirms the smoke cases **fail** on the
stock GPT model, so a green run is meaningful. It's headless, deterministic, and CI-runnable. Renode
validates register/protocol *logic* only — flash P/E timing and the RAM-residency fetch-stall hazard,
CAN bit-timing, USB/DFU, real UART baud/framing, and clock tolerance stay HIL-bench-only, and the
drivers' ICU event numbers and SCI2 line settings remain BENCH-VERIFY items. Emulation complements,
never replaces, the bench.

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
  ra4m1-hal/         from-scratch MIT/Apache application HAL (M3): GPIO, clocks,
                     GPT/PWM, UART, SPI, I2C, ADC, DAC — embedded-hal 1.0 + async;
                     examples/ per peripheral; bootloader never depends on it
bootloader/          the bootloader binary crate: boot decision, drivers (flash-LP,
                     CAN0, USB rail, USBFS), tasks (can_update, usb_dfu, led)
demo-app/            Embassy blinky linked at 0x8000, used to exercise the update path
renode/              Renode emulation harness (M2.5, done): custom C# peripheral
                     models + 4 Robot suites (smoke/analog/serial/bootloader),
                     12 cases green on Renode v1.16.1
scripts/             build-out.sh / build-out.ps1 — export .elf/.bin/.hex into out/
docs/
  plan.md                        implementation plan — locked decisions, milestones
  plan-m2.5-renode.md            M2.5 Renode emulation plan
  plan-m3-hal.md                 M3 application-HAL plan
  research/          behavior spec, ecosystem findings, PAC-coverage audit
  bench/             on-hardware bench checklists (m0-spikes.md)
  README.md          docs index
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

## Emulation (Renode) — run the suites

The M2.5 harness in [`renode/`](renode/) boots the real `ra4m1-hal` example ELFs and the bootloader
ELF in the [Renode](https://github.com/renode/renode) emulator — **no hardware needed** — and proves
the drivers, the boot flow, and the CAN/flash protocols work against register-level models. It's a
test-time tool only; Renode is never linked into firmware. **12 cases across 4 suites**
(`smoke`/`analog`/`serial`/`bootloader`) pass on the pinned Renode **v1.16.1**. Full details, and the
Windows/WSL local-run recipe, are in [`renode/README.md`](renode/README.md).

**Prerequisites:** Renode **v1.16.1** (pinned). `renode-test` needs `robotframework` + `psutil` on
its Python. The custom C# models compile at runtime — no Renode rebuild.

```sh
# 1. Build the ELFs the suites boot
cargo build --release --target thumbv7em-none-eabihf -p ra4m1-hal --examples
cargo build --release --target thumbv7em-none-eabihf -p bootloader

# 2. Automated pass/fail (all four Robot suites)
<renode-install>/renode-test renode/smoke.robot renode/analog.robot renode/serial.robot renode/bootloader.robot
#   → all 12 cases ... status OK
```

> **Path caveat:** Renode's monitor can't tokenize paths with spaces/parentheses, so `renode-test`
> won't run from a checkout under a path like `…/Work ( Normal Corporation )/…`. Run it from a
> space-free path (or in CI, which uses a clean checkout). The suites themselves are path-clean —
> see [`renode/README.md`](renode/README.md) for the exact copy-out steps.

The smoke suite asserts behaviours that **fail** on Renode's stock GPT model and **pass** with the
custom `Ra4m1Gpt320` model (a negative control), so a green run is meaningful. Renode validates
driver *logic* only — see the honesty boundaries in [`renode/README.md`](renode/README.md).

## CI

`.github/workflows/ci.yml` runs on **`v*` tag pushes (releases)** and manual `workflow_dispatch`
only — no longer on every branch push or PR:

- `cargo fmt --all -- --check`
- `cargo clippy -- -D warnings` (firmware members on the target triple, `boot-logic` on the host)
- `cargo test -p boot-logic` (host)
- `cargo build --release` for `thumbv7em-none-eabihf`, then a hard **32,768 B** size gate on
  `bootloader.bin` and a byte-check of the OFS0/OFS1/Security-MPU option bytes against the stock
  dump (brick-risk region — any mismatch fails the build)
- a **`renode` job** (`needs: firmware-build`) that runs the four Robot suites in the pinned
  `antmicro/renode:1.16.1` container — the hardware-free integration gate (see
  [`renode/README.md`](renode/README.md))
- Build artifacts (`.bin`/`.elf`/`.hex`) are uploaded

Pushing a `v*` tag additionally publishes a GitHub Release with those artifacts and
auto-generated notes.

## Further reading

- [`docs/plan.md`](docs/plan.md) — the implementation plan: locked architectural decisions,
  stack choices, workspace layout, concurrency architecture, and the milestone/unit breakdown.
- [`docs/plan-m2.5-renode.md`](docs/plan-m2.5-renode.md) — plan for M2.5, hardware-free
  validation of the release ELFs in the Renode emulator.
- [`docs/plan-m3-hal.md`](docs/plan-m3-hal.md) — plan for M3, a from-scratch MIT/Apache-2.0
  RA4M1 HAL for applications (not the bootloader), replacing the LGPL community options.
- [`docs/research/ra4m1-pac-coverage.md`](docs/research/ra4m1-pac-coverage.md) — the PAC-coverage
  audit (spike S-B): which RA4M1 peripheral blocks the `ra4m1` PAC exposes (all typed).
- [`renode/README.md`](renode/README.md) — the Renode emulation harness: the custom GPT model, the
  smoke test, and how to run it.
- [`docs/research/master-feature-spec.md`](docs/research/master-feature-spec.md) — the
  implementation-independent behavior spec (what the C bootloader does, and every hardware
  constant it relies on).
- [`docs/research/embassy-ra4m1-ecosystem.md`](docs/research/embassy-ra4m1-ecosystem.md) —
  ecosystem findings (PAC choice, prior art, Embassy crate versions).
- [`docs/bench/m0-spikes.md`](docs/bench/m0-spikes.md) — on-hardware bench checklist for the M0
  bring-up spikes.
