# Implementation Plan — M3: LGPL-free RA4M1 Application HAL

**Contract for implementation dispatches.** This is the design plan for **Milestone 3**: a
from-scratch, MIT/Apache-2.0 hardware-abstraction layer for the Renesas **R7FA4M1AB**, built for
*applications* running on this chip — not for the bootloader. It is a sibling of `docs/plan.md`
(cited below as **PLAN §n**) and shares its citation conventions: behaviour context in
`docs/research/master-feature-spec.md` (**SPEC §n**), ecosystem findings in
`docs/research/embassy-ra4m1-ecosystem.md` (**ECO §n**). Where this plan and `docs/plan.md`
conflict, `docs/plan.md`'s locked decisions win for the bootloader; this document governs only
the new HAL crate and its examples.

Target: Arduino UNO R4 Minima, Renesas **R7FA4M1AB** (Cortex-M4F, 48 MHz HOCO, 256 KB code flash,
8 KB data flash, 32 KB SRAM). Triple `thumbv7em-none-eabihf`. probe-rs chip `R7FA4M1AB` (ECO §6).

**PLAN ONLY.** No HAL code is written by this milestone's authoring step. The deliverable is this
document, reviewed and signed off (the Open Questions in §9) before any unit dispatches.

---

## 0. Why M3 exists (the framing)

The bootloader (M0–M2, shipped) has its own register-level drivers and must not be destabilized
or bloated by this work. Applications for this chip, however, have **no HAL to build on**: the
Embassy project maintains no RA HAL (ECO §1), and the two community options that come closest are
both **LGPL-3.0-or-later** and therefore incompatible with this repo's shipped MIT/Apache
commercial firmware:

- `ra4-hal` / `ra4-rs` (the Embassy-README-listed community HAL) — LGPL-3.0-or-later, and it
  lacks CAN, USB, and flash regardless (ECO §1).
- `ra4m1-ctpac` (the chiptool PAC underneath `ra4-hal`) — LGPL-3.0-or-later (ECO §2).

The research phase already banned LGPL crates for exactly this reason (PLAN §1.6: "no LGPL deps
at all, which also excludes `ra4m1-ctpac`"). **M3 is the LGPL-free alternative, built from
scratch.** Its whole justification is licensing: an application HAL this repo can ship without
inheriting the LGPL compliance burden that static linking of `ra4-hal` would impose.

This is not greenfield. The chip already has a proven, on-silicon PAC-level base in this repo
(`crates/ra4m1-embassy`) and three bench-proven register-level drivers in `bootloader/src/drivers/`.
M3 leverages both — see the reuse ledger (§8).

---

## 1. Locked decisions & their derived constants

These are the decisions this plan proposes as locked for M3. Each is also surfaced in §9 for the
user's sign-off before any unit dispatches.

1. **Licensing: MIT OR Apache-2.0, LGPL-free — the reason M3 exists.** Every direct and
   transitive dependency is MIT and/or Apache-2.0. This bans `ra4-hal`, `ra4-rs`, and
   `ra4m1-ctpac` from the graph (ECO §1/§2) and inherits the workspace license
   (`Cargo.toml` `[workspace.package] license = "MIT OR Apache-2.0"`). Dom Wilson's
   `dw-labs-org/UNO-R4-rust` (MIT, ECO §5/§7) remains usable as a *register cross-reference*, the
   same way `bootloader/src/drivers/can0.rs` used it — reference, not dependency.

2. **New crate `crates/ra4m1-hal`, layered on the existing base.** The full application HAL is a
   **new** workspace member. It depends on the existing `crates/ra4m1-embassy` for the shared
   low-level base (clocks, PFS, the GPT320 `embassy-time` driver, VBTBKR, the `device.x`
   interrupt-vector mechanism) and adds the peripheral-driver surface on top. The **bootloader
   does not depend on `ra4m1-hal`** and never pulls in HAL bulk — its 32 KB size gate (PLAN §1.2)
   stays safe by construction. Rationale and the alternatives considered: §4.1.

3. **Exactly one `embassy-time` driver in any binary — the existing GPT320 one.** `embassy-time`
   permits a single `time_driver_impl!` per binary. `ra4m1-embassy/src/time_driver.rs` already
   fills that slot (GPT320, 3 MHz, IEL0). **`ra4m1-hal` ships no second time driver.** Apps get
   time by initializing the base driver (re-exported through the HAL for ergonomics); the HAL's
   own delay types are thin wrappers over `embassy-time`. Derived constraint: GPT channel 0
   (GPT320) is reserved for the time base and is **not** offered as an app PWM/capture channel by
   the HAL (§4.4).

4. **PAC: `ra4m1` 0.2** — the workspace-locked choice (`Cargo.toml` `[workspace.dependencies]
   ra4m1 = "0.2"`; PLAN §1.6/§2, ECO §2). Known coverage gaps (flash-LP/FACI absent, USBFS PHY
   registers absent) are already worked around with contained raw-MMIO modules in the bootloader
   (`flash_lp.rs`, `usbfs.rs`); the HAL adds one more contained raw-MMIO seam of the same shape
   for PFS-style register maps the PAC types too narrowly (§4.6). No new PAC is introduced.

5. **`embedded-hal` 1.0 + `embedded-hal-async` at every driver boundary.** Drivers expose the
   standard traits (blocking + async) plus `embedded-io`/`embedded-io-async` for serial, so they
   are portable and host-mockable against `embedded-hal-mock` (rules/embedded.md, rules/testing.md).
   Per-peripheral trait list: §4.3.

6. **Async where a completion event exists; blocking where it does not.** UART/SPI/I2C/ADC are
   genuinely async (interrupt/DTC + waker); DAC and GPIO writes are blocking register writes. This
   echoes the bootloader's deliberate polled-CAN + blocking-flash choices (PLAN §4) — the reasons
   there were determinism and no-executor leaf routines; here the reason is symmetric: await a real
   hardware event, never a poll loop (rules/embassy.md). Per-peripheral: §4.5.

7. **Peripheral phasing, foundation-first:** GPIO → timers/PWM (GPT) → serial buses
   (UART/SPI/I2C) → analog (ADC/DAC), as phased sub-milestones M3.1–M3.4. Ordering rationale:
   §4.2 and §6.

Unchanged and inherited without restatement: the HOCO 48 MHz clock tree and its rate constants
(`clocks::{ICLK_HZ, PCLKA_HZ, PCLKB_HZ, PCLKC_HZ, PCLKD_HZ, FCLK_HZ}`), the PWPR-locked PFS
pin-mux helpers (`pfs::{set_gpio_output, set_peripheral_function, reset_to_gpio_input}`), the
GPT320 tick rate (`embassy-time-driver/tick-hz-3_000_000`), and the ICU/NVIC single-priority-table
discipline (all in `crates/ra4m1-embassy`, PLAN §4).

---

## 2. Stack decisions

- **PAC: `ra4m1` 0.2** (ra-rs/ra, svd2rust, MIT OR Apache-2.0) — workspace-pinned, already proven
  on silicon by M0–M2. `ra4m1-fsp-pac` (same lineage, MIT/Apache, fresher) remains the documented
  fallback if a spike finds a peripheral register block missing that the fallback SVD covers
  (ECO §2); the swap is mechanical (PLAN §2).
- **Base crate: `crates/ra4m1-embassy`** (this repo) — clocks, PFS, GPT320 time driver, VBTBKR,
  `device.x`/`build.rs` interrupt-vector plumbing. The HAL depends on it; see §8 reuse ledger.
- **Embassy crates** at the workspace-frozen pins (`Cargo.toml` `[workspace.dependencies]`, verified
  at U0.1 time 2026-07-16 — note these superseded PLAN §2's original candidates): `embassy-executor
  0.10`, `embassy-time 0.5`, `embassy-time-driver 0.2`, `embassy-sync 0.8`, `embassy-futures 0.1`.
  The HAL uses `embassy-sync` for waker/signal state and `embassy-time` for delays; it does **not**
  depend on `embassy-executor` (drivers are executor-agnostic — an app brings its own executor).
- **Trait crates (new to the workspace, added by M3.0):** `embedded-hal 1.0`,
  `embedded-hal-async 1.0`, `embedded-io 0.6`, `embedded-io-async 0.6`, and `embedded-hal-bus`
  (for `SpiDevice`/`I2cDevice` sharing). Exact versions pinned and verified against crates.io at
  M3.0, mirroring PLAN §2's "verify current versions" discipline. The existing `embedded-can 0.4`
  and `embedded-storage 0.3` pins stay as-is (bootloader owns those seams).
- **Runtime/logging:** `cortex-m 0.7`, `cortex-m-rt 0.7` (`device` feature — the HAL inherits the
  base crate's hand-written `__INTERRUPTS` table, PLAN §2 / `time_driver.rs`), `defmt 1`,
  `defmt-rtt 1`, `panic-probe 1`, `static_cell 2`, `heapless 0.9` — all workspace-pinned.
- **`critical-section 1`** with `critical-section-single-core` (single-core chip) — the one sound
  RawMutex flavour for ISR↔task sharing on this part (rules/embassy.md; `time_driver.rs` header).
- Toolchain: stable, pinned via `rust-toolchain.toml`. Runner: `probe-rs run --chip R7FA4M1AB`.
  `DEFMT_LOG=info` default.
- **No 32 KB size gate on the HAL or its examples** — that gate is the bootloader's (PLAN §1.2) and
  is kept safe precisely by decision §1.2 (HAL bulk never enters the bootloader binary). HAL
  examples link at the app base `0x0000_8000` (PLAN §1.2, `demo-app/memory.x`) and are sized only
  by the app slot's 227,328 B.

---

## 3. Workspace layout

The M3 additions to PLAN §3's tree (unchanged members elided). `crates/ra4m1-hal` is new; the root
`Cargo.toml` re-opens once at **M3.0** to add the member and the shared trait deps (the same way
U0.1 owned the root manifest — PLAN §6):

```
Cargo.toml                     # M3.0 adds crates/ra4m1-hal to members + embedded-hal(-async)/io deps
crates/
  boot-logic/                  # UNCHANGED (pure bootloader logic; unrelated to the HAL)
  ra4m1-embassy/               # BASE — UNCHANGED by M3 except possibly one re-export (§4.7); do not destabilize
    src/{clocks,pfs,time_driver,vbtbkr}.rs
    device.x, build.rs
  ra4m1-hal/                   # NEW — the application HAL
    Cargo.toml                 # depends on ra4m1-embassy (base) + ra4m1 (PAC) + embedded-hal(-async)/io
    src/lib.rs                 # mod tree + re-exports of base clocks/time init (created complete at M3.0)
    src/interrupt.rs           # bind_interrupts!-style macro + central IELSR slot-allocation table (§4.7)
    src/gpio.rs                # M3.1 — PORT/PFS pins: InputPin/OutputPin/StatefulOutputPin + async Wait
    src/gpt.rs                 # M3.2 — GPT PWM/capture channels (ch1..): SetDutyCycle, async capture
    src/sci.rs                 # M3.3 — SCI common: UART (embedded-io[-async]); SCI-SPI/SCI-I2C if used
    src/spi.rs                 # M3.3 — SPI bus: SpiBus + async SpiBus
    src/i2c.rs                 # M3.3 — IIC bus: I2c + async I2c
    src/adc.rs                 # M3.4 — ADC14: async scan-complete + blocking one-shot
    src/dac.rs                 # M3.4 — DAC12: blocking channel write
    src/pac_glue.rs            # contained raw-MMIO for register maps the PAC types too narrowly (§4.6)
    examples/                  # cargo examples, one per peripheral phase (§7)
      blinky_gpio.rs
      pwm_gpt.rs
      uart_echo.rs
      spi_loopback.rs
      i2c_scan.rs
      adc_read.rs
    memory.x                   # FLASH ORIGIN 0x0000_8000 (app base), RAM 32K — mirrors demo-app/memory.x
demo-app/                      # UNCHANGED (bootloader's blinky demo); may later adopt ra4m1-hal (§7, Q)
bootloader/                    # UNCHANGED — never depends on ra4m1-hal (decision §1.2)
```

`ra4m1-hal/src/lib.rs` is created **complete** at M3.0 (all `mod` lines declared against stub
files) so later phase units only fill their own claimed file — the same shared-file trap
discipline PLAN §6 enforces. Any phase unit adding a dependency edits only `ra4m1-hal/Cargo.toml`;
the root manifest is frozen again after M3.0.

`memory.x` for the HAL examples and any example-runner config start from `demo-app/`'s equivalents
(the app-base linker script already exists and is proven).

---

## 4. Architecture

### 4.1 Crate layout — the recommendation

Three framings were considered (the user deferred this decision to this plan):

**(a) New `crates/ra4m1-hal`, sharing the base.** A separate HAL crate; the bootloader's
`ra4m1-embassy` stays minimal and the 32 KB gate stays safe; the two share low-level modules
(clocks/PFS/time driver).

**(b) Grow `ra4m1-embassy` into the full HAL.** Fewer crates. But the bootloader already depends on
`ra4m1-embassy`; every peripheral driver added there must be feature-gated off for the bootloader
build or it silently pulls HAL bulk into the 32 KB binary. Feature-gating an entire HAL surface is
fragile — one missing `#[cfg(feature = ...)]` on a `pub use` re-opens the size regression, and CI
would need a dedicated "bootloader with default features" size job to catch it. This couples the
recovery-critical bootloader's dependency graph to unrelated application-HAL churn — exactly the
destabilization the framing forbids.

**(c) Extract a common base crate both depend on.** Pull clocks/PFS/PAC-glue/time-driver into a
small base that the bootloader and a new HAL both consume.

**Recommendation: (a), which is already (c).** `crates/ra4m1-embassy` *is* the extracted common
base — its map responsibility is literally "Shared RA4M1 chip runtime consumed by both firmware
binaries," and both binaries already depend on it. The clean move is therefore: **keep
`ra4m1-embassy` as the thin shared base (essentially untouched), and add `crates/ra4m1-hal` on
top of it** (framing a, resting on the extraction of framing c that already happened in M0). This
satisfies the repo's own principles — "Co-locate Before Extracting" and "Fork Intentionally"
(rules/principles.md): the shared low-level bits are genuinely shared across two consumers now
(bootloader + HAL), so their extraction is justified rather than premature; and clocks/PFS live in
exactly one place, so no accidental fork.

Framing (b) is rejected: coupling the bootloader's graph to HAL growth risks the size gate for a
crate-count saving that isn't worth it.

**Naming smell (flagged, Q1):** `ra4m1-embassy` is a slightly awkward name for a crate that is
really the PAC-glue + time-driver *base* the HAL builds on. Renaming it to `ra4m1-base` (or
`ra4m1-pac-glue`) would read better — but renaming touches the bootloader's imports and risks
destabilizing shipped M0–M2 code, which the framing forbids. Recommendation: **keep the name**,
document the base role in its crate docs, and treat a rename as an optional later cleanup, not part
of M3.

### Dependency graph

```
                         ra4m1 (PAC, MIT OR Apache-2.0)
                        /        |                    \
                       /         |                     \
          crates/ra4m1-embassy   |          bootloader/src/drivers/*
          (BASE: clocks, pfs,    |          (can0, flash_lp, usbfs,
           time_driver GPT320,   |           usb_rail — bootloader-only)
           vbtbkr, device.x)     |                     |
              |         \        |                     |
              |          \       |                     |
              |     crates/ra4m1-hal (NEW)             |
              |     (gpio, gpt, sci, spi, i2c,         |
              |      adc, dac, interrupt macro)        |
              |      + embedded-hal(-async)/io traits  |
              |          |                             |
          bootloader     |                         (bootloader binary:
          (BASE + own     \                         BASE + own drivers,
           drivers ONLY,   \                        NEVER ra4m1-hal)
           32 KB gate)   ra4m1-hal/examples/*
                         + future apps
                         (BASE via HAL + ra4m1-hal)

  crates/boot-logic (pure, no HAL/PAC/executor) — orthogonal, consumed by bootloader only.
```

Key edges: the **bootloader** binary depends on `ra4m1-embassy` + `ra4m1` + its own drivers, and
**not** on `ra4m1-hal` (size gate safe). **Apps/examples** depend on `ra4m1-hal`, which depends on
`ra4m1-embassy`, so there is exactly one copy of clocks/PFS/time-driver in any app binary and
exactly one `embassy-time` driver (decision §1.3). `ra4m1` PAC is a shared leaf.

### 4.2 Peripheral phasing — the recommendation and its rationale

| Phase | Peripheral group | Why here |
|---|---|---|
| **M3.1** | **GPIO** (PORT + PFS) | Foundation and most-depended-on: every other peripheral's I/O pins route through PFS pin-mux, so GPIO/pin ownership types are a prerequisite for the buses and PWM. Smallest, best-understood surface (PFS raw-MMIO base already exists). Retires the pin-ownership/type-state design before anything depends on it. |
| **M3.2** | **Timers / PWM** (GPT) | Self-contained and PAC-covered (the time driver already drives a GPT block). Depends on GPIO for GTIOC pin routing. Proves the `SetDutyCycle` trait shape and GPT channel ownership (channel 0 reserved for time, §1.3) before the larger bus surface. |
| **M3.3** | **Serial buses** (UART/SCI, SPI, I2C/IIC) | The biggest and most valuable surface, and the first genuinely-async work (interrupt/DTC + waker). Depends on GPIO (pin mux) and the M3.0 interrupt-binding macro (waker wiring). Grouped because SCI/SPI/IIC share the async-transfer + waker pattern; done after the pattern is de-risked by a spike. |
| **M3.4** | **Analog** (ADC14, DAC12) | Last: least commonly the app blocker, and the trait story is weakest (`embedded-hal` 1.0 has no stable ADC trait — §4.3), so this phase carries the most bespoke-API design and benefits from landing after the async pattern is settled. ADC is async (scan-complete IRQ + waker); DAC is blocking. |

Foundation-first, most-depended-on-first. Later phases (a **Phase 5 / future**, out of M3's core
scope but named so the plan covers the full chip): RTC, WDT/IWDT, hardware CRC, DTC/DMAC helper
surface (if not already needed by M3.3), and ELC/ICU event-link helpers. CAN0 and USBFS already
have bootloader drivers (`can0.rs`, `usbfs.rs`) as reference patterns; promoting them into the HAL
is a future item, not M3 (§8).

### 4.3 Trait surface

`embedded-hal` 1.0 (blocking) + `embedded-hal-async` at every boundary, `embedded-io`/`-async` for
serial, so drivers are portable and mock cleanly against `embedded-hal-mock` (rules/testing.md).
Which traits each peripheral implements:

| Peripheral | Blocking traits | Async traits | Notes |
|---|---|---|---|
| GPIO pin | `digital::{InputPin, OutputPin, StatefulOutputPin, ErrorType}` | `digital::Wait` (edge/level) | Async wait backed by an ICU IRQn pin event + waker (spike S-D). |
| GPT PWM | `pwm::SetDutyCycle`, `pwm::ErrorType` | — | Duty/period are register writes; no completion event to await. |
| GPT capture | — | inherent `async fn capture()` + waker | No standard eh trait for capture; inherent async method. |
| UART (SCI) | `embedded-io::{Read, Write, ErrorType}` | `embedded-io-async::{Read, Write}` | The idiomatic serial seam (matches Dom Wilson's SCI, ECO §7). |
| SPI | `spi::{SpiBus, ErrorType}` | `spi-async::SpiBus` | `SpiDevice` via `embedded-hal-bus` (CS + bus sharing). |
| I2C (IIC) | `i2c::{I2c, ErrorType}` | `i2c-async::I2c` | `I2cDevice` via `embedded-hal-bus`. |
| ADC14 | inherent one-shot read | inherent `async fn read()`/`scan()` + waker | `embedded-hal` 1.0 has **no** ADC trait (the 0.2 trait was dropped); expose a documented inherent API, revisit if an ecosystem trait stabilizes (Q). |
| DAC12 | inherent `set_value()` | — | No eh DAC trait; inherent blocking API. |
| Delay | `delay::DelayNs` | `delay-async::DelayNs` | Thin wrapper over `embassy-time` (decision §1.3), not a new time source. |

Every driver has its own error enum (`#[derive(Debug, defmt::Format)]`, rules/rust.md) that
implements the trait's `Error`/`ErrorKind` mapping. No `unwrap`/`panic` on fallible paths.

### 4.4 GPT channel allocation (derived from §1.3)

`embassy-time` owns GPT channel 0 (GPT320, the 3 MHz free-running counter, `time_driver.rs`). The
HAL therefore offers **GPT channels 1..N** for app PWM/capture and must refuse channel 0 at the
type level (a channel-ownership token that never yields ch0). The exact channel count and which are
32-bit vs 16-bit on this part is a PAC/reference-manual fact to confirm in spike S-B — do not
assume (the time driver's docs note GPT320 is the confirmed 32-bit channel; the rest are unverified
here). Bench LED-fade in the bootloader uses "GPT ch3" (PLAN §4, `led.rs`), which is evidence more
channels exist, but the HAL must verify the full map before publishing channel tokens.

### 4.5 Async model per peripheral

Async where a real completion event exists, blocking otherwise (decision §1.6), echoing the
bootloader's reasoning (PLAN §4):

- **Genuinely async (interrupt/DTC + waker):** UART RX/TX, SPI transfer, I2C transfer, ADC
  scan-complete. Each awaits a hardware completion/RX-ready event via a `Signal`/waker set from a
  minimal ISR (the `time_driver.rs` `on_interrupt` shape: clear the latch, wake, return). Whether
  the SCI/SPI FIFO depth forces per-byte interrupts or the **DTC** (Data Transfer Controller) is
  needed for block transfers is a genuine unknown → spike **S-C**.
- **Blocking (register write, no event to await):** DAC `set_value`, GPIO output write, GPT duty
  set. These are single-register operations; making them async would be theatre.
- **Bootloader parallel (not adopted here, noted for symmetry):** the bootloader chose *polled*
  CAN + *blocking* flash for determinism and executor-free leaf routines (PLAN §4). The HAL is the
  opposite context — app code under an executor — so it defaults to awaiting a real event, per
  rules/embassy.md's "don't poll where you can await."

### 4.6 PAC coverage vs raw-MMIO

`ra4m1` 0.2 coverage, from what M0–M2 established plus what M3 must verify (do not invent register
facts — cite or mark "verify in spike S-B"):

| Peripheral | PAC coverage | Evidence / action |
|---|---|---|
| PORT (direction/output/input) | Believed covered | Verify in S-B; GPIO driver uses PAC PORT regs where typed. |
| PFS (pin-mux) | Typed too narrowly (per-pin registers, no `(port,pin)` generic) | **Reuse `pfs.rs`'s contained raw-MMIO** computed-address helper (already proven, §8). |
| GPT | Covered | `time_driver.rs` drives the `gpt320` register block via the PAC. |
| SCI (UART/SPI/I2C modes) | Believed covered | Dom Wilson's SCI UART built on `ra4m1` PAC (ECO §5/§7); verify block presence in S-B. |
| IIC (dedicated I2C) | Unverified | Verify in S-B; raw-MMIO fallback of the `flash_lp.rs`/`usbfs.rs` shape if absent. |
| SPI (dedicated SPI block) | Unverified | Verify in S-B. |
| ADC14 / DAC12 | Unverified | Verify in S-B. |
| Flash-LP / FACI | **Absent** | Bootloader's `flash_lp.rs` uses raw MMIO; not an M3 concern (flash is the bootloader's). |
| USBFS PHY | **Absent** | Bootloader's `usbfs.rs` uses raw MMIO; not an M3 core concern. |

Any raw-MMIO the HAL needs is contained in `pac_glue.rs` (or the specific driver module) with a
`// SAFETY:` comment and the R01UM0007 section cited — the identical discipline as `pfs.rs`,
`flash_lp.rs`, and `usbfs.rs` (rules/embedded.md). No scattered raw pointers.

### 4.7 Embassy integration & interrupt binding

**Time (decision §1.3):** the HAL re-exports the base driver's init (`ra4m1_embassy::time_driver::init`)
and, optionally, adds one thin re-export line to `ra4m1-embassy/src/lib.rs` so an app can pull the
time base through the HAL without naming the base crate. That single-line re-export is the **only**
contemplated edit to the base crate; if review prefers zero base-crate churn, the app names
`ra4m1_embassy::time_driver::init` directly (Q). No `time_driver_impl!` appears in `ra4m1-hal`.

**Interrupt binding:** the base crate already owns the interrupt-vector mechanism — a hand-written
240-slot `__INTERRUPTS` table with the 32 real ICU vectors (`time_driver.rs`), `device.x` providing
weak `PROVIDE(IELn = DefaultHandler)` aliases, and a strong `#[export_name = "IELn"]` symbol
overriding the weak default (IEL0 = the time-driver alarm). The HAL's async peripherals need more
of those IELn slots (UART/SPI/I2C/ADC wakers). Two options:

- **A `bind_interrupts!`-style macro (recommended).** `ra4m1-hal` provides a macro that, given a
  peripheral + its ICU event number, generates the strong `#[export_name = "IELn"]` handler (clear
  the IELSR IR latch, wake the peripheral's waker, return — the `time_driver.rs::on_interrupt`
  shape) and wires the IELSR event-link config. This is the Embassy-idiomatic surface (matches how
  every Embassy HAL and `ra4-hal` bind interrupts, ECO §1) and keeps app-facing wiring declarative.
- **Per-driver strong symbols (fallback).** Each driver hand-exports its own `IELn` like the time
  driver does. Simpler to write, worse to use (the app must hand-allocate slots).

**The architectural tension (→ spike S-A):** the IELSR slot allocation is a *central* resource
(rules/embedded.md: one central priority scheme). Today `time_driver.rs` hard-owns IEL0 and pads
the rest with `DefaultHandler`. A macro that lets apps claim IEL1..IEL31 must (1) not collide with
IEL0, (2) reconcile with the base crate's `__INTERRUPTS` table (which is a `static` in the base
crate — can app-side strong symbols override its `DefaultHandler` padding via the `device.x` weak
`PROVIDE`s? that is exactly the mechanism, but it must be proven end-to-end), and (3) record every
claimed slot + NVIC priority in one place so priorities don't invert against IEL0's deliberately
lowest setting (`time_driver.rs` priority table). Spike S-A retires this before M3.3 (the first
phase that needs app IRQs). The central slot/priority table lives in `ra4m1-hal/src/interrupt.rs`
and cross-links the base crate's IEL0 entry.

**No `embassy-executor` dependency in the HAL** (decision §2): drivers are executor-agnostic;
async methods return futures an app's executor drives. This keeps the HAL usable from any Embassy
executor configuration and mockable on the host.

---

## 5. Technical spikes (ordered; each retires a risk before dependent phases build on it)

Each spike lands as a small commit on a scratch path and reports pass/fail before dependent units
dispatch (PLAN §5 discipline). Spikes here are cheaper than M0's — the board bring-up, clocks,
time driver, and probe-rs/RTT path are all already proven.

- **S-A — interrupt-binding macro + central IELSR allocation.** Prove an app-side strong
  `#[export_name = "IEL1"]` (or a macro that generates it) overrides the base crate's
  `DefaultHandler` padding via `device.x`'s weak `PROVIDE`, fires on a chosen ICU event, and wakes
  a task — without disturbing IEL0 (the time driver). Establish the central slot/priority table
  shape. **Pass:** a second IRQ fires and wakes a task while `embassy-time` still ticks; priorities
  recorded centrally. **Blocks M3.3/M3.4.** Route: embassy-pro + arm-cortex-expert.
- **S-B — PAC coverage audit.** Compile-check that `ra4m1` 0.2 exposes the register blocks M3
  needs (PORT, GPT ch1..N with their 16/32-bit widths, SCI, IIC, SPI, ADC14, DAC12) and record
  which are typed usably vs need a raw-MMIO `pac_glue.rs` seam. **Pass:** a per-peripheral
  coverage table with each block marked covered / narrow / absent; fallback to `ra4m1-fsp-pac`
  decided per-block if a needed block is absent from both SVDs. Route: arm-cortex-expert.
- **S-C — async serial transfer path (DTC vs per-byte IRQ).** Bring up one SCI in UART mode with
  an interrupt/waker RX and TX; determine whether the SCI FIFO depth is adequate or the **DTC** is
  required for block transfers, and prototype the waker wiring against S-A's macro. **Pass:** a
  byte echoes host↔device under the executor with no busy-wait; the DTC-vs-IRQ decision recorded
  with a rationale. **Blocks the SPI/I2C sub-units** (they reuse the pattern). Route: embassy-pro +
  arm-cortex-expert.
- **S-D — async GPIO edge wait.** Wire one ICU IRQn pin event to a waker so `digital::Wait`
  resolves on an edge. Small; may ride along with S-A. **Pass:** a button-edge future resolves
  once per edge, no bounce-loop. Route: embassy-pro.

S-B can run first and in parallel (pure compile audit, no hardware). S-A gates the async phases.
S-C/S-D validate the two async patterns (transfer-complete vs pin-event) M3.3/M3.4 depend on.

---

## 6. Milestones & implementation units

**Shared-file rule (Rust trap, PLAN §6):** **M3.0** creates `ra4m1-hal/Cargo.toml`,
`src/lib.rs`, and every stub module file **complete** (all `mod` lines up front) and edits the
root `Cargo.toml` once (add member + trait deps). After M3.0 the root manifest is frozen again;
each phase unit fills only its own claimed file(s) and edits only `ra4m1-hal/Cargo.toml` for
member-local deps.

Agent routing (per the task's dispatch rules): **register bring-up → arm-cortex-expert**;
**trait/type/API design → rust-pro / rust-engineer**; **Embassy/interrupt/waker integration →
embassy-pro**. Phases with mixed work name the lead + support.

### M3.0 — HAL scaffold & interrupt foundation

| Unit | Description | Files | Routing |
|---|---|---|---|
| **U3.0.1** | New crate skeleton: root `Cargo.toml` gains `crates/ra4m1-hal` + `embedded-hal 1.0`/`embedded-hal-async`/`embedded-io`/`-async`/`embedded-hal-bus` deps; `ra4m1-hal/{Cargo.toml, src/lib.rs}` with the complete `mod` tree + stub files, `examples/` dir, `memory.x` from `demo-app/`. Acceptance: `cargo build` (target) on stubs, `cargo clippy -- -D warnings` clean; bootloader build unchanged (no new deps in its graph). | root `Cargo.toml`, `crates/ra4m1-hal/{Cargo.toml,src/lib.rs,memory.x}` + stubs | rust-engineer |
| **U3.0.2** | Spike **S-A** + **S-D** → production `interrupt.rs`: the `bind_interrupts!`-style macro, the central IELSR slot/priority table (cross-linking IEL0), and the async-GPIO edge-wait proof folded into the GPIO async path. Optional one-line time re-export in `ra4m1-embassy/src/lib.rs` (or none — Q2). Acceptance = S-A/S-D pass criteria. | `crates/ra4m1-hal/src/interrupt.rs`, (maybe) `crates/ra4m1-embassy/src/lib.rs` | embassy-pro (+ arm-cortex-expert) |
| **U3.0.3** | Spike **S-B**: PAC coverage audit; produces the per-peripheral coverage table that later phases key off. No production code beyond a throwaway compile probe. Acceptance = S-B pass criteria. | (report only; scratch commit) | arm-cortex-expert |

### M3.1 — GPIO

| Unit | Description | Files | Routing |
|---|---|---|---|
| **U3.1.1** | `gpio.rs`: pin-ownership types (a pin token that proves ownership, splittable from PAC PORT singletons), blocking `InputPin`/`OutputPin`/`StatefulOutputPin` over the PORT regs + `pfs.rs` mux, async `digital::Wait` via the S-D edge-wait wiring. Per-pin error enum. Reuses `pfs::{set_gpio_output, reset_to_gpio_input}` (§8). | `crates/ra4m1-hal/src/gpio.rs`, `examples/blinky_gpio.rs` | rust-pro (types) + arm-cortex-expert (PORT regs) + embassy-pro (Wait) |

### M3.2 — Timers / PWM (GPT)

| Unit | Description | Files | Routing |
|---|---|---|---|
| **U3.2.1** | `gpt.rs`: GPT channel-ownership tokens for ch1..N (channel 0 reserved for time, §4.4), `SetDutyCycle` PWM on GTIOCn pins (routed via `pfs::set_peripheral_function`), optional async input capture. Period/duty math as host-testable pure functions. Reuses `led.rs`'s C-parity GPT register recipe as a reference (§8). | `crates/ra4m1-hal/src/gpt.rs`, `examples/pwm_gpt.rs` | arm-cortex-expert (regs) + rust-pro (traits/math) |

### M3.3 — Serial buses (UART / SPI / I2C)

| Unit | Description | Files | Routing |
|---|---|---|---|
| **U3.3.1** | `sci.rs`: SCI UART — blocking `embedded-io::{Read,Write}` + async `embedded-io-async` via the S-C transfer path (interrupt/DTC + waker), baud-divisor as a host-tested pure function. Depends on S-A macro + S-C decision. | `crates/ra4m1-hal/src/sci.rs`, `examples/uart_echo.rs` | arm-cortex-expert (SCI regs) + embassy-pro (async/waker) |
| **U3.3.2** | `spi.rs`: blocking `SpiBus` + async `spi-async::SpiBus`, `SpiDevice` via `embedded-hal-bus`; reuses the S-C transfer/waker pattern. | `crates/ra4m1-hal/src/spi.rs`, `examples/spi_loopback.rs` | arm-cortex-expert + embassy-pro |
| **U3.3.3** | `i2c.rs`: blocking `I2c` + async `i2c-async::I2c` over the IIC block (or SCI-I2C per S-B), `I2cDevice` via `embedded-hal-bus`. | `crates/ra4m1-hal/src/i2c.rs`, `examples/i2c_scan.rs` | arm-cortex-expert + embassy-pro |

U3.3.2/U3.3.3 parallelize after U3.3.1 proves the shared async-transfer pattern.

### M3.4 — Analog (ADC / DAC)

| Unit | Description | Files | Routing |
|---|---|---|---|
| **U3.4.1** | `adc.rs`: ADC14 async scan-complete (IRQ + waker via S-A macro) + blocking one-shot; bespoke inherent API (no eh ADC trait, §4.3); calibration/scaling math host-tested. | `crates/ra4m1-hal/src/adc.rs`, `examples/adc_read.rs` | arm-cortex-expert + embassy-pro |
| **U3.4.2** | `dac.rs`: DAC12 blocking channel write, bespoke inherent API. | `crates/ra4m1-hal/src/dac.rs` | arm-cortex-expert + rust-pro |

### Future (named, out of M3 core scope, §4.2)

Phase 5 — RTC, WDT/IWDT, hardware CRC, DTC/DMAC helper surface, ELC/ICU event-link helpers; and
promotion of the bootloader's `can0.rs`/`usbfs.rs` register work into HAL drivers (§8). Planned as
a later milestone; not dispatched by M3.

**M3 exit criteria (on the bench):** each phase's example runs on silicon under a normal Embassy
executor (GPIO blinky/edge-wait, GPT PWM, UART echo, SPI loopback, I2C scan, ADC read) with
`embassy-time` still ticking; `cargo clippy -- -D warnings` clean; host tests green; and the
**bootloader binary is byte-for-byte unaffected** (its dependency graph never gained `ra4m1-hal`).

---

## 7. Verification strategy

Per-unit gates (every HAL unit): `cargo fmt --check`, `cargo clippy -- -D warnings`,
`cargo build --release` for `thumbv7em-none-eabihf`. No 32 KB gate (that is the bootloader's, §2).

- **Host tests (`embedded-hal-mock` at trait boundaries, rules/testing.md):** the register I/O
  itself is not host-testable, but the *pure sub-logic* is, and that is where the bugs hide —
  UART/SCI baud-divisor computation, GPT period/duty arithmetic, ADC calibration/scaling, I2C
  address framing. Those are `#[cfg(test)]` pure functions with golden vectors. Any driver logic
  that is generic over an `embedded-hal(-async)` trait (e.g. a `SpiDevice`-consuming helper) is
  tested against `embedded-hal-mock` at that seam — never mocking registers (rules/testing.md).
- **Async-with-time host tests:** timeout/retry policies in async drivers are host-tested with
  `embassy-time`'s `std`/mock driver (rules/testing.md) — no hardware needed for a timeout policy.
- **HIL smoke (probe-rs, per peripheral):** each phase's `examples/*.rs` doubles as its smoke
  test, run via `probe-rs run`; the spike pass criteria (S-A..S-D) re-run as smoke steps. Kept as
  scripted notes in the unit PRs, not a test framework — flash time is the budget (rules/testing.md).
  On-target `embedded-test`/`defmt-test` only for what genuinely needs the chip (interrupt
  binding, DTC transfers) and kept few.
- **Demo/example app:** the existing `demo-app` is the *bootloader's* end-to-end demo (Embassy
  blinky at `0x8000`, VBTBKR CAN-entry trigger — PLAN §6 U1.6) and should stay bootloader-focused
  to keep M1's E2E stable. **A dedicated set of HAL examples (`crates/ra4m1-hal/examples/`) is
  warranted** — HALs conventionally ship per-peripheral examples, they link at the app base, and
  cargo examples cost no extra workspace member. Whether `demo-app` later adopts `ra4m1-hal` to
  exercise it under the real boot flow is an open item (Q4), not required for M3 exit.
- **defmt as the diagnostic story** (rules/embedded.md): `DEFMT_LOG`-filtered logs at driver
  boundaries; `Debug2Format` only off the hot path.

---

## 8. Reuse ledger — what already exists and how M3 leverages it

M3 is explicitly not greenfield. Every row is proven on silicon by M0–M2.

| Existing asset | Where | How M3 uses it |
|---|---|---|
| HOCO 48 MHz clock tree + rate constants | `ra4m1-embassy/src/clocks.rs` | Base dependency; the HAL derives peripheral clock rates (baud, GPT prescale, ADC clock) from `PCLKA/B/C/D_HZ` const-asserts, exactly as `can0.rs` derives CAN bit timing. Not re-implemented. |
| PWPR-locked PFS pin-mux (raw-MMIO, `(port,pin)`-generic) | `ra4m1-embassy/src/pfs.rs` | GPIO reuses `set_gpio_output`/`reset_to_gpio_input`; every bus/PWM driver reuses `set_peripheral_function` for pin routing. The one contained raw-MMIO PFS exception is already built (§4.6). |
| GPT320 `embassy-time` driver (3 MHz, IEL0) | `ra4m1-embassy/src/time_driver.rs` | Fills the single `embassy-time` slot (§1.3); the HAL re-exports its `init` and reserves GPT ch0. The `on_interrupt` shape (clear latch → wake → return) is the template for every HAL waker ISR. |
| VBTBKR backup-register access | `ra4m1-embassy/src/vbtbkr.rs` | Available to apps that want the bootloader-entry handoff (e.g. an app requesting CAN update); not modified. |
| `device.x` weak-`PROVIDE` + strong-`#[export_name]` interrupt mechanism + `__INTERRUPTS` table | `ra4m1-embassy/{device.x, src/time_driver.rs}`, `build.rs` | The exact substrate the HAL's `bind_interrupts!`-style macro binds against (§4.7, spike S-A). Not duplicated — extended. |
| CAN0 driver — peripheral-handle ownership, bounded polls, `embedded-can::Frame`, `// SAFETY:`+R01UM0007 cites, clock-derived const-asserts | `bootloader/src/drivers/can0.rs` | **Reference pattern** for HAL bus drivers: how to own a peripheral singleton, cite the manual, const-assert timing from the clock tree. Not imported (bootloader-only). |
| flash-LP driver — RAM-resident routines, contained raw-MMIO for an absent PAC block, `embedded-storage` trait impls | `bootloader/src/drivers/flash_lp.rs` | **Reference pattern** for the `pac_glue.rs` raw-MMIO seam and for implementing an `embedded-*` trait over raw registers with a per-driver error enum. |
| USBFS driver — waker/ISR model, `embassy-usb-driver` impl, W0C status-register handling, register-recipe map to a C reference | `bootloader/src/drivers/usbfs.rs` | **Reference pattern** for the async waker model (the deepest example in-repo of an interrupt-driven Embassy driver on this chip) and for porting a C register recipe faithfully. |
| Pure-logic host-test discipline | `crates/boot-logic` | The model for the HAL's host-tested pure sub-logic (baud/duty/calibration math) — same `#[cfg(test)]` + golden-vector approach. |
| App-base linker script | `demo-app/memory.x` | Template for `ra4m1-hal/memory.x` (examples link at `0x0000_8000`, PLAN §1.2). |

---

## 8b. Requirements & rules traceability

M3 has no SPEC requirement list of its own (SPEC governs the bootloader); its "requirements" are
this repo's binding rules and the task's deferred decisions. Traceability:

| Design decision | Satisfies |
|---|---|
| New crate, bootloader never depends on it (§1.2, §4.1) | rules/agent-directives.md "Strict Scoping"; the framing's "must not destabilize/bloat the bootloader"; PLAN §1.2 size gate. |
| One `embassy-time` driver, the existing GPT320 (§1.3) | rules/embassy.md "exactly one time-driver implementation is enabled." |
| `embedded-hal 1.0`/`-async`/`embedded-io` at every boundary (§1.5, §4.3) | rules/embedded.md driver-trait-boundary rule; rules/testing.md host-mockability. |
| Pure sub-logic host-tested; no HAL in it (§7) | rules/rust.md "Layout & Seams"; rules/testing.md. |
| No LGPL deps; MIT/Apache only (§1.1) | PLAN §1.6; the framing (the reason M3 exists). |
| Contained raw-MMIO with `// SAFETY:`+R01UM0007 cites (§4.6) | rules/embedded.md PAC/raw-MMIO containment; rules/rust.md `unsafe` discipline. |
| No `static mut`; `StaticCell`/`embassy-sync`/atomics for shared state (throughout) | rules/rust.md; rules/embassy.md sharing order. |
| Central IELSR slot/priority table, no priority inversion vs IEL0 (§4.7) | rules/embedded.md "priority scheme documented centrally." |
| Async awaits a real event; blocking only where none exists (§1.6, §4.5) | rules/embassy.md "don't poll where you can await," "never block inside a task." |
| `defmt` diagnostics, `Debug2Format` off hot path (§7) | rules/rust.md logging; rules/embedded.md. |
| Per-driver `#[derive(Debug, defmt::Format)]` error enum, `?` propagation (§4.3) | rules/rust.md "Error Handling." |

---

## 9. Open questions & assumptions

1. **Crate name `ra4m1-embassy` as the base (Q).** It reads awkwardly as the base a HAL sits on
   (§4.1). *Proposal:* keep the name (renaming risks destabilizing shipped M0–M2 imports, which the
   framing forbids); document the base role; treat a rename to `ra4m1-base` as an optional later
   cleanup. **Needs sign-off: keep the name, or rename now?**
2. **One-line time re-export into the base crate (Q).** §4.7 contemplates adding a single
   `pub use` to `ra4m1-embassy/src/lib.rs` so apps pull time through the HAL. That is the *only*
   contemplated base-crate edit. *Proposal:* allow it (one line, additive). **Needs sign-off:
   permit the one-line base-crate edit, or require apps to name `ra4m1_embassy::time_driver::init`
   directly (zero base-crate churn)?**
3. **Interrupt-binding macro vs per-driver strong symbols (Q, → spike S-A).** §4.7 recommends a
   `bind_interrupts!`-style macro; the fallback is per-driver `#[export_name]`. *Proposal:* macro,
   pending S-A proving app-side strong symbols override the base crate's `DefaultHandler` padding
   via `device.x`. **Needs sign-off after S-A.**
4. **Does `demo-app` adopt the HAL (Q)?** Keeping `demo-app` bootloader-focused protects M1's E2E;
   a HAL-exercising app would prove the HAL under the real boot flow. *Proposal:* ship HAL
   `examples/` for M3; consider a separate HAL demo app later, leave `demo-app` alone. **Needs
   sign-off: examples-only, or a new HAL demo app in M3?**
5. **DTC vs per-byte interrupts for async serial (A, → spike S-C).** The SCI FIFO depth and whether
   the DTC is needed for block transfers is unverified. *Proceeding on:* S-C decides before M3.3;
   per-byte interrupt is the known-simple fallback if DTC bring-up proves heavy.
6. **PAC register-block coverage for SCI/IIC/SPI/ADC/DAC (A, → spike S-B).** Not verified here (no
   register facts invented). *Proceeding on:* S-B audits; `ra4m1-fsp-pac` fallback per-block, then a
   contained raw-MMIO `pac_glue.rs` module (the `flash_lp.rs`/`usbfs.rs` pattern) if a needed block
   is absent from both SVDs.
7. **GPT channel count and 16/32-bit widths (A, → spike S-B).** Only GPT320 (ch0, 32-bit) is
   confirmed (`time_driver.rs`); the LED task uses "ch3," implying more, but the full map is
   unverified. *Proceeding on:* S-B enumerates before the HAL publishes channel tokens; ch0 is
   reserved for time regardless.
8. **No stable `embedded-hal` ADC trait (A).** `embedded-hal` 1.0 dropped the 0.2 ADC trait; the
   HAL exposes a bespoke inherent ADC API (§4.3). *Proceeding on:* revisit if an ecosystem ADC
   trait stabilizes; the inherent API is documented and stable in the meantime.
9. **Trait-crate versions (A).** `embedded-hal 1.0`, `embedded-hal-async 1.0`, `embedded-io 0.6`,
   `embedded-io-async 0.6`, `embedded-hal-bus` — pinned and verified against crates.io at M3.0
   (PLAN §2's "verify current versions" discipline). *Proceeding on:* pin at M3.0.
10. **Phase 5 / full-chip coverage (A).** RTC, WDT, hardware CRC, DTC/DMAC, ELC, and CAN/USB
    promotion are named (§4.2) but out of M3's core scope. *Proceeding on:* planned as a later
    milestone; M3 delivers GPIO→GPT→buses→analog.
