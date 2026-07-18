# Implementation Plan — Rust/Embassy CAN Bootloader (UNO R4 Minima, RA4M1)

**Contract for implementation dispatches.** Behavior spec: `docs/research/master-feature-spec.md`
(cited below as **SPEC §n** / requirement **Rn**). Ecosystem findings:
`docs/research/embassy-ra4m1-ecosystem.md` (cited as **ECO §n**). Where this plan and SPEC
conflict, the locked decisions in §1 win. Everything else in SPEC's 43-item checklist (SPEC §10)
is binding.

> **Milestone 3 (application HAL)** is planned separately — see `docs/plan-m3-hal.md` (summarized
> under §6, "M3").

Target: Arduino UNO R4 Minima, Renesas **R7FA4M1AB** (Cortex-M4F, 48 MHz HOCO, 256 KB code
flash, 8 KB data flash, 32 KB SRAM). Triple `thumbv7em-none-eabihf`. probe-rs chip `R7FA4M1AB`
(ECO §6).

---

## 1. Locked decisions & their derived constants

1. **Embassy single-slot bootloader.** `embassy-executor` runs inside the bootloader; a CAN
   update task (M1) and later a USB DFU task (M2) write directly into one app slot. NOT
   embassy-boot, no A/B swap, no rollback (overrides ECO §3's Option A recommendation).
2. **Bootloader region grows 16 KB → 32 KB.** New code-flash map (supersedes SPEC §1.1 / R1):

   | Region | Range | Size | 2 KB blocks |
   |---|---|---|---|
   | Bootloader | `0x0000_0000`–`0x0000_7FFF` | 32 KB (`BOOT_SIZE 0x8000`) | 16 |
   | App slot | `0x0000_8000`–`0x0003_F7FF` | 227,328 B (`APP_SLOT_SIZE 0x0003_7800`) | 111 |
   | Metadata block | `0x0003_F800`–`0x0003_FFFF` | 2 KB (`METADATA_ADDR 0x0003_F800`) | 1 |

   Derived deltas, each traceable to this decision:
   - `APP_BASE = 0x8000`; VTOR/MSP/entry from `0x8000`/`0x8004` (supersedes R5, R12).
   - Size gate: bootloader `.bin` ≤ **32,768 B**, hard build failure (supersedes R2).
   - SIZE command bounds: reject 0 and > **227,328** (supersedes R20).
   - HELLO ACK bytes 4..7 announce `0x0003_7800` LE (format per R19 unchanged;
     `can_flash.py` only warns on a non-`0x3B800` value and enforces the announced size —
     SPEC §4.8 — so the host tool keeps working unmodified).
   - ERASE: metadata block first, then **111** app blocks = 112 total (supersedes R21).
   - Seq-rewind uniqueness still holds: window-start seqs have period 128 (86·k mod 256,
     SPEC §4.4) and 111 ≤ 128.
   - M2 DFU descriptor becomes `@CodeFlash /0x00000000/16*2Ka,112*2Kg` (supersedes R36).
   - **Consequence (flagged):** stock Arduino IDE sketches link at `0x4000` and can no longer
     boot; every app (demo app, Airform) must link at `0x8000`. Accepted per the locked decision.
3. **Metadata format unchanged**: 16-byte LE record `{magic 0xB007D00D, image_size,
   image_crc32, header_crc32}` at `0x0003_F800`, written last (SPEC §3.1, R15). No changes.
4. **CAN Update Protocol v1 wire-compatible** (SPEC §4 in full): IDs `0x79E`/`0x79F` +
   v2-config overrides, 250 kbit/s, DLC-8, commands `0x01..0x07`, windowed ACK per 2 KB block,
   error codes, INFO 5-frame reply. VBTBKR handoff magics unchanged (`0x07738135` double-tap,
   `0xB007CAFE` CAN entry; SPEC §1.4, R6–R8).
5. **USB DFU is Milestone 2.** M1 reserves the task/memory hooks (§4) but ships zero USB code
   beyond the transceiver-rail power-up sequence (SPEC §5.3, R33 — that is a power fix, not USB).
6. **PAC: `ra4m1` (svd2rust, MIT OR Apache-2.0)** — see §2. No `ra4-hal` (LGPL, and lacks CAN,
   flash, USB anyway — ECO §1); no LGPL deps at all, which also excludes `ra4m1-ctpac`.

Unchanged from SPEC and binding without restatement: OFS0 `0xFFFFFFFF`/OFS1 `0xFFFFCEDF` at
`0x400`/`0x404` + Security-MPU words + ID code all-`0xFF` (SPEC §1.5, R4); boot decision order
(SPEC §2, R6–R13); data-flash read gate (SPEC §2.4, R34); CAN register recipe (SPEC §5.2,
R27–R32); USB-rail power-up (SPEC §5.3, R33); DFU config block at `0x4010_1C00` (SPEC §6.2,
R40); the ten hardware gotchas (SPEC §9.1).

---

## 2. Stack decisions

- **PAC: `ra4m1` 0.2** (ra-rs/ra, svd2rust, MIT OR Apache-2.0). Rationale: it is the only PAC
  with proven prior art on this exact chip — dw-labs-org/UNO-R4-rust's clocks, interrupts, and
  MIT-licensed CAN0 driver are all built on it (ECO §2, §5), so our primary register
  cross-reference compiles against our PAC. `ra4m1-fsp-pac` (same generator lineage, fresher
  2026 release) is the designated fallback if the 2022-vintage `ra4m1` fails Spike S0's
  compile/coverage check; both are MIT/Apache so the swap is mechanical.
- **Embassy crates** per `.claude/template/other/Cargo.toml`: `embassy-executor 0.7`
  (`arch-cortex-m`, `executor-thread`, `defmt`), `embassy-time 0.4`, `embassy-sync 0.6`,
  `embassy-futures 0.1` — verify current versions on crates.io at U0.1 time; the
  `embassy-time-driver` API crate version must match the pinned `embassy-time`.
- **No Embassy HAL exists for RA** (ECO §1): drivers are hand-written against the PAC, contained
  per rules/embedded.md (one module per peripheral, `// SAFETY:` + RA4M1 HW manual R01UM0007
  section cites).
- Runtime/logging: `cortex-m 0.7` (`critical-section-single-core` — single-core chip),
  `cortex-m-rt 0.7`, `defmt 1`, `defmt-rtt 1`, `panic-probe 1`, `static_cell 2`, `heapless 0.8`.
- Trait seams: `embedded-can` types for frames (ECO §5); the flash driver exposes an
  `embedded-storage` `NorFlash`-shaped blocking API (read/erase/program) so M2 DFU and any
  future embassy-boot experiment reuse it unchanged.
- Toolchain: stable, pinned via `rust-toolchain.toml` (template). Runner:
  `probe-rs run --chip R7FA4M1AB`. `DEFMT_LOG=info` default.
- Profile: `opt-level = "z"`, `lto = "fat"`, `codegen-units = 1`, `debug = 2` (symbols stay off
  flash). `cargo size` gate at 32,768 B from day one (ECO §8's advice).

---

## 3. Workspace layout

Cargo workspace (multi-binary ⇒ shared lib crates, rules/embedded.md):

```
Cargo.toml                     # [workspace] members = crates/*, bootloader, demo-app
rust-toolchain.toml            # from .claude/template/other/rust-toolchain.toml
.cargo/config.toml             # from .claude/template/other/cargo-config.toml
crates/
  boot-logic/                  # PURE: no_std, zero PAC/HAL/executor deps, host-testable (R41)
    Cargo.toml
    src/lib.rs                 # mod lines only — created complete in U0.1 (shared-file rule §6)
    src/memory_map.rs          # §1 constants + const-asserts (partition consistency)
    src/crc32.rs               # SPEC §3.2; golden vector test
    src/image_meta.rs          # metadata codec + validation state machine (SPEC §3.1/§3.3)
    src/protocol.rs            # CAN update engine: phases, windowing, replies (SPEC §4)
    src/dfu_config.rs          # v1/v2 config-block parse (SPEC §6.2)
  ra4m1-embassy/               # shared chip runtime for both binaries (PAC-level, contained)
    Cargo.toml
    src/lib.rs
    src/clocks.rs              # HOCO 48 MHz, ICLK 48 / PCLKA 48 / PCLKB 24 / FCLK 24 (SPEC §5.2)
    src/time_driver.rs         # custom embassy-time driver (Spike S2 decides AGT/GPT/SysTick)
    src/vbtbkr.rs              # PRC1-unlocked VBTBKR read/clear/write (SPEC §1.4)
    src/pfs.rs                 # PWPR-locked pin-function writes, PSEL-before-PMR (SPEC §5.2)
bootloader/
  Cargo.toml
  build.rs                     # memory.x plumbing
  memory.x                     # FLASH 0x0 len 32K, RAM 0x2000_0000 len 32K, _stext past OFS
  src/main.rs                  # boot decision (pre-executor) + executor start + spawns
  src/board.rs                 # board seam per .claude/template/embassy/board.rs
  src/ofs.rs                   # OFS0/OFS1/Security-MPU words emitted into .ofs section
  src/flash_read.rs            # volatile reads of code flash at address 0 (raw ptrs, never refs)
  src/drivers/mod.rs
  src/drivers/flash_lp.rs      # code+data flash P/E from RAM; data-flash read gate (SPEC §2.4)
  src/drivers/can0.rs          # register-level CAN0, FIFO-mailbox mode (SPEC §5.2)
  src/drivers/usb_rail.rs      # SCKE + VDCEN + 5 ms soak (SPEC §5.3)
  src/drivers/usbfs.rs         # M2 only: embassy-usb-driver impl
  src/tasks/mod.rs
  src/tasks/can_update.rs      # protocol session task
  src/tasks/led.rs             # parked-mode GPT ch3 PWM fade on P111 (SPEC §2.5)
  src/tasks/usb_dfu.rs         # M2 only
demo-app/
  Cargo.toml
  memory.x                     # FLASH ORIGIN 0x0000_8000 LENGTH 0x37800, RAM 32K
  src/main.rs                  # Embassy blinky + defmt version banner; VBTBKR CAN-entry trigger
.github/workflows/ci.yml       # pinned toolchain, clippy -D warnings, host tests, size gate, releases
```

`boot-logic` is `#![no_std]` with `std` only under `#[cfg(test)]`; its APIs take caller-provided
byte buffers or an incremental-read trait (`trait ImageRead { fn read(&self, offset: u32, buf:
&mut [u8]) -> Result<..> }`) — **never** slices conjured at address 0. The bootloader implements
that trait in `flash_read.rs` with volatile raw-pointer copies (SPEC §9.1.7: references to
address 0 are UB in Rust; contained unsafe with `// SAFETY:`).

Templates: `bootloader/src/main.rs` and `board.rs` start from `.claude/template/embassy/main.rs`
and `board.rs` (spawn-and-park shape, board seam); `Cargo.toml` / `.cargo/config.toml` /
`memory.x` / `rust-toolchain.toml` start from `.claude/template/other/`. `frame.rs`/`link.rs`
templates are reference shape for the CAN task's channel plumbing, not copied verbatim.

---

## 4. Concurrency architecture

**Boot decision runs before the executor starts.** Steps 1–5 of SPEC §2 (VBTBKR checks,
double-tap window, validation, jump) are plain blocking code in `main` before
`embassy_executor::Executor` exists — the app-boot path never pays executor startup, and the
jump sequence (R12) never has to tear an executor down. The 500 ms double-tap window uses a
`cortex_m::asm::delay`-derived wait at 48 MHz — an allowed exception to the no-busy-wait rule
because no executor is running yet (documented at the call site). GO exits via
`NVIC_SystemReset` (R13), so the executor, once started, only ever dies by reset.

Tasks (thread-mode executor only; no interrupt executor — nothing here has a hard latency
requirement the 4-deep RX FIFO + windowed protocol doesn't already absorb):

| Task | Owns | Yields on |
|---|---|---|
| `can_update` | CAN0 driver, flash-LP driver, protocol engine, 2 KB window buffer | RX poll `Ticker` (see below); one erase block per loop iteration during ERASE |
| `led` (parked mode) | GPT ch3 PWM on P111 | 100 ms `Ticker` (SPEC §2.5 fade step) |
| `usb_dfu` (M2) | USBFS driver, embassy-usb device | USB events |

Sharing: **M1 has none** — each task owns its hardware outright (narrowest primitive: move).
M2 adds one `Mutex<NoopRawMutex, FlashLp>` (single executor) shared between `can_update` and
`usb_dfu`, guard never held across an await (flash ops are synchronous critical sections anyway).

**Two documented blocking exceptions** (the protocol is designed around them, SPEC §9.1.4):
- Flash P/E: the CPU cannot fetch from code flash during program/erase (ECO §4) — each
  block-erase or block-program is a synchronous, IRQ-masked, RAM-resident critical section
  (~ms). The windowed protocol guarantees no CAN frame arrives mid-program (mid-window frames
  are unACKed; host stops-and-waits). ERASE chunks one block per task-loop iteration so the
  LED/USB tasks stay live between blocks (R21, R39).
- CAN TX `SENTDATA` wait: bounded poll ≤ ~200 ms worst case (SPEC §5.2). v1 keeps it
  synchronous inside `can_update` (one in-flight reply, nothing else to starve in CAN-only
  mode); if parked-mode fairness measurements object, convert to an awaited retry loop.

**CAN RX is ticker-paced polling, not interrupt-driven** — a deliberate, named deviation:
at 250 kbit/s a DLC-8 frame occupies ≥ ~444 µs on the wire and the FIFO is 4 deep, so a
`CAN_RX_POLL = 500 µs` ticker (named constant, sized for 250 kbit/s + 4-deep FIFO) that
**drains the whole FIFO each tick** can never lose frames, while interrupt-driven RX would
require bringing up the RA ICU event-link (IELSR) mapping — real work with no protocol benefit. Power is irrelevant (board is bus/USB powered
in update mode). Revisit only if M2 profiling demands it.

Interrupts used at all: exactly one — the time-driver compare interrupt (S2). Its ICU/NVIC slot
and priority are recorded in `ra4m1-embassy/src/time_driver.rs` as the project's single
priority table. The jump sequence gains one step over SPEC §2.2: stop/module-stop the
time-driver counter alongside SysTick (R12 extension, noted in `main.rs`).

Timeout/watchdog story (rules/embassy.md): the bootloader is a server — it parks forever
awaiting host commands by design, so session awaits carry **no** timeout (matches C behavior;
the host owns retry/timeout budgets, SPEC §4.8). OFS0 keeps the IWDT off (R4). The one internal
timeout is the CAN TX bounded poll above.

---

## 5. Technical spikes (ordered; each retires a risk before real units build on it)

Each spike lands as a small commit on a scratch path (`bootloader/` skeleton is fine) and its
pass/fail is reported before dependent units dispatch.

- **S0 — probe-rs + RTT + clocks + PAC sanity.** Minimal `cortex-m-rt` binary at `0x0`:
  clock bring-up to HOCO 48 MHz (PCLKB 24, FCLK 24), defmt-RTT banner, LED on.
  Flash via `probe-rs run --chip R7FA4M1AB`; then restore stock via
  `docs/reference/dfu_minima.hex` (on `main`) over SWD to prove the recovery path (ECO §6).
  Also: confirm the `ra4m1` PAC compiles on the pinned stable toolchain and exposes every
  register block we need (CAN0, flash/FACI-LP, SYSTEM incl. VBTBKR/PRCR, USBFS, PORT/PFS,
  AGT/GPT, MSTP). **Pass:** defmt output seen; stock bootloader restored; all register blocks
  present. **Fail →** swap PAC to `ra4m1-fsp-pac`; if flash registers are missing from both
  SVDs, flash access becomes a contained raw-MMIO module (arm-cortex-expert).
- **S1 — OFS/option-byte emission.** `memory.x` `PROVIDE(_stext = ...)` past `0x440` + an
  `.ofs` section carrying OFS0 `0xFFFFFFFF`, OFS1 `0xFFFFCEDF`, Security-MPU words `0x408–0x43F`
  byte-identical to the stock dump in `docs/build.md` (on `main`) (SPEC §1.5, R4).
  **Pass:** `objcopy`'d binary bytes at `0x400..0x43F` diff clean against the expected dump,
  and the S0 binary still runs (IWDT did not auto-start). This is brick-risk work: route
  arm-cortex-expert, review like unsafe code.
- **S2 — Embassy executor + custom time driver + footprint.** Implement `embassy-time-driver`
  against AGT or GPT via the PAC (candidate order: 32-bit GPT channel if the PAC confirms one,
  else AGT with wrap handling, else SysTick-at-1-kHz as the known-simple fallback); two tasks,
  500 ms `Ticker` blink + defmt timestamps. **Pass:** measured tick accuracy within HOCO
  tolerance (±~1–2 %); `cargo size` of executor+time+defmt skeleton ≤ 20 KB (leaves ≥ 12 KB
  for CAN + flash + protocol in the 32 KB gate). **Fail on size →** strip defmt from release,
  re-measure; executor-in-bootloader is the locked architecture, so size must be made to fit.
- **S3 — flash-LP P/E from RAM.** Erase + program + read-back a scratch block high in the app
  slot while executing, IRQs masked, routines forced RAM-resident
  (`#[link_section = ".data"]` + `#[inline(never)]`, ECO §4). Repeat ≥ 10 cycles.
  **Pass:** golden pattern verifies every cycle, no lockup, works with a debugger detached.
- **S4 — CAN0 exchange with the real host tool.** Port the SPEC §5.2 register recipe (reset →
  BCR/MBM → halt mailbox config → operate → RFE; FIDCR/MKR exact-match filter; MCTL double
  `0x00` clear), including the S5-order rail power-up (USBFS module-start, SCKE bounded wait,
  VDCEN, 5 ms soak — SPEC §5.3) since bench boards need it before any CAN access.
  **Pass:** device NAKs/ACKs visible in `candump` at 250 k and answers an unmodified
  `can_flash.py --info` probe (empty name is fine at this stage).
- **S5 — VBTBKR across software reset.** PRC1-unlocked write of `0xB007CAFE` to `VBTBKR[4..7]`,
  `NVIC_SystemReset()`, read-back+clear after reset. **Pass:** magic survives exactly one
  reset and reads cleared after the clear. (Small; may ride along with S0 hardware time.)

The data-flash read gate (SPEC §2.4) is not a spike — it is a specified port with a known
recipe — but it lands inside U1.1 with the flash driver and gets a bench check there.

---

## 6. Milestones & implementation units

**Shared-file rules (Rust traps):** U0.1 creates every crate's `Cargo.toml`, `lib.rs`/`main.rs`,
and `mod.rs` files **complete** — all `mod` lines declared up front against stub files — so
later units only fill their own claimed files. Units marked **[main.rs]** also edit
`bootloader/src/main.rs` (spawns/boot flow) and must be dispatched serially with respect to each
other. Any unit adding a dependency edits only its own crate's `Cargo.toml`; the workspace root
`Cargo.toml` is frozen after U0.1.

### M0 — Scaffolding & risk retirement

| Unit | Description | Files | Routing |
|---|---|---|---|
| **U0.1** | Workspace scaffolding from templates: root `Cargo.toml`, `rust-toolchain.toml`, `.cargo/config.toml` (triple + `probe-rs run --chip R7FA4M1AB`), all crate skeletons with complete `mod` trees and stub files, `.gitignore`, CI skeleton (fmt+clippy+host-test+size-gate jobs). Acceptance: `cargo build` (target) and `cargo test` (host, `boot-logic`) both succeed on stubs; `cargo clippy -- -D warnings` clean. | everything in §3's tree (stubs), `.github/workflows/ci.yml` | rust-engineer |
| **U0.2** | Spikes S0 + S1 + S5 (board bring-up, OFS emission, VBTBKR): `clocks.rs`, `vbtbkr.rs`, `pfs.rs`, `ofs.rs`, `memory.x`, `build.rs`, minimal `main.rs`/`board.rs`. Acceptance = the spike pass criteria. **[main.rs]** | `crates/ra4m1-embassy/src/{clocks,vbtbkr,pfs}.rs`, `bootloader/{memory.x,build.rs,src/main.rs,src/board.rs,src/ofs.rs}` | arm-cortex-expert |
| **U0.3** | Spike S2: `time_driver.rs`, executor skeleton with two toy tasks, `task-arena-size` set deliberately (named, sized for 3 tasks + margin), `cargo size` recorded. Acceptance = S2 pass criteria. **[main.rs]** | `crates/ra4m1-embassy/src/time_driver.rs`, `bootloader/src/main.rs`, `bootloader/Cargo.toml` | embassy-pro |
| **U0.4** | `boot-logic`: `memory_map.rs` (§1 constants + const-asserts: partitions tile 256 KB, APP_BASE block-aligned, slot = 111 blocks), `crc32.rs` (golden vector `0xCBF43926`, R14), `image_meta.rs` codec + validation with the incremental `ImageRead` seam (R10/R11/R15: every error code, fallback SP-check, order-of-checks tests). Host tests per SPEC §3.3's failure modes. | `crates/boot-logic/src/{memory_map,crc32,image_meta}.rs` | rust-pro |
| **U0.5** | `boot-logic`: `protocol.rs` — the full SPEC §4 engine (phases, HELLO-from-any-phase, windowing/seq/rewind, ERR codes, reply builder incl. HELLO slot-size `0x37800`) plus `dfu_config.rs` (v1/v2/invalid parse, per-ID fallback, R40). Host tests: every NAK path, window rewind, seq wrap over a full 111-block image, HELLO-restart, DLC≠8, foreign-ID silence, v1/v2/corrupt config (R41). Engine emits abstract effects (`EraseAll`, `ProgramBlock{..}`, `ComputeCrc{..}`, `WriteMetadata`, `Reset`) — the task interprets them, so flash never leaks into logic. | `crates/boot-logic/src/{protocol,dfu_config}.rs` | rust-pro |

U0.2/U0.3 serialize on `main.rs`; U0.4/U0.5 are host-only and run in parallel with anything.

### M1 — CAN bootloader at parity (minus USB)

| Unit | Description | Files | Routing |
|---|---|---|---|
| **U1.1** | Flash-LP driver (grows S3 into production): NorFlash-shaped API, RAM-resident P/E, 8-byte write units, read-back verify, **data-flash read gate** (Open+Reset success + DFLEN re-assert + ≥ 6 µs settle, latch, SPEC §2.4/R34), never-write-below-`0x8000` guard (R3). Bench: S3 loop + gate check after a software reset with a dirtied FACI. | `bootloader/src/drivers/flash_lp.rs`, `bootloader/src/flash_read.rs` | arm-cortex-expert |
| **U1.2** | CAN0 driver (grows S4): init(bitrate, rx_filter_id) accepting only 250 k/500 k, non-blocking `try_receive` (BOST → RFEST → MB28 window → RFPCR `0xFF` → RFMLF clear), bounded `send`, bus-off re-init (R31), deinit restoring MSTP + PFS reset (R32), plus `usb_rail.rs` (R33). Uses `embedded-can` frame types. | `bootloader/src/drivers/can0.rs`, `bootloader/src/drivers/usb_rail.rs` | arm-cortex-expert |
| **U1.3** | Boot decision + jump: SPEC §2 order exactly — double-tap magic, CAN magic, non-POR 500 ms arming window, validation via `boot-logic` over `flash_read`, SP-word fallback, `boot5` jump sequence (+ time-driver counter stop; R12), fail-closed park. **[main.rs]** | `bootloader/src/main.rs`, `bootloader/src/board.rs` | embassy-pro |
| **U1.4** | `can_update` task: drive the U0.5 engine's effects against U1.1/U1.2 — rail power-up first in CAN-only entry, config-ID load behind the data-flash gate, RX poll ticker, window buffer (2048 B, task-owned), ERASE chunked per block, INFO 5-frame reply, GO → deinit → reset. CAN-init failure: reset in CAN-only mode, degrade in parked mode (R35). **[main.rs]** (spawn wiring) | `bootloader/src/tasks/can_update.rs`, `bootloader/src/main.rs` | embassy-pro |
| **U1.5** | Parked-mode LED fade task: GPT ch3 PWM on P111, 100 ms step (SPEC §2.5). Spawned only in parked mode. **[main.rs]** (spawn wiring) | `bootloader/src/tasks/led.rs`, `bootloader/src/main.rs` | embassy-pro |
| **U1.6** | Demo app: Embassy blinky at `0x8000` with defmt banner + version-identifiable blink rate; optional VBTBKR CAN-entry trigger on a grounded pin; `cargo objcopy` recipe for the `.bin` that `can_flash.py` flashes. | `demo-app/{Cargo.toml,memory.x,src/main.rs}` | embassy-pro |
| **U1.7** | CI hardening + release: size gate **32,768 B** hard failure, `cargo size` printed per build, host tests, clippy `-D warnings`, artifacts (`.bin`/`.elf`/`.hex`), tag-driven releases (R43, SPEC §8's carry-forwards). | `.github/workflows/ci.yml` | rust-engineer |

Order: U1.1/U1.2 parallel after M0; U1.3 → U1.4 → U1.5 serialized on `main.rs` (or dispatched
as one unit if a single agent takes them); U1.6/U1.7 parallel to anything after U0.1.

**M1 exit criteria (end-to-end, on the bench):** flash bootloader via SWD → board parks (no
valid app) → unmodified `can_flash.py` flashes the demo app end-to-end incl. `--info` → GO →
demo app blinks → double-tap reset re-enters the bootloader → re-flash succeeds → power-loss
table SPEC §4.5 spot-checked (kill power after ERASE; board parks, never bricks). R42 satisfied
except `--enter-bootloader` (needs an Airform app; verified by writing the CAN magic from the
demo app's trigger pin instead — same VBTBKR path).

### M2 — USB DFU

Planned now, dispatched after M1 ships. The M1 hooks it relies on: parked mode already spawns
from a task list, the flash driver is NorFlash-shaped behind a mutex-ready seam, and the 32 KB
region was sized for a USB stack.

| Unit | Description | Files | Routing |
|---|---|---|---|
| **U2.1** | USBFS `embassy-usb-driver` implementation for RA4M1 — the largest single unknown in the whole plan (zero prior art, ECO §1/§7; ra4-hal left it unimplemented). Bring-up against embassy-usb's CDC example shape before DFU. | `bootloader/src/drivers/usbfs.rs` | arm-cortex-expert + embassy-pro |
| **U2.2** | DFU 1.1 function on embassy-usb: VID `0x2341`/PID `0x0369`, descriptor `@CodeFlash /0x00000000/16*2Ka,112*2Kg` + `@DataFlash` alt, detach→reset flow, upload + download, strings from the config block (R36–R37, SPEC §6.1–6.2). Evaluate reusing `embassy-usb-dfu` vs hand-rolling the class (its updater is embassy-boot-coupled — open question Q7). | `bootloader/src/tasks/usb_dfu.rs` | embassy-pro |
| **U2.3** | Combined parked mode: `usb_dfu` + `can_update` + `led` coexisting; flash mutex; one erase/program block per IRQ-off window so USB never starves past a block (R39); DFU-download metadata write-back with the zero/oversize guards (R38, SPEC §6.3). **[main.rs]** | `bootloader/src/tasks/usb_dfu.rs`, `bootloader/src/tasks/can_update.rs`, `bootloader/src/main.rs` | embassy-pro |

M2 exit: Arduino-IDE-style DFU detach-upload works; size gate still green; M1 E2E re-run clean.

### M3 — Application HAL (LGPL-free RA4M1 HAL)

A separate track for *applications* on this chip — not the bootloader. The community `ra4-hal`
and `ra4m1-ctpac` are LGPL-3.0 (§1.6 banned them), so M3 builds an MIT/Apache alternative from
scratch: a new `crates/ra4m1-hal` layered on the existing `ra4m1-embassy` base
(clocks/PFS/time-driver reused), with `embedded-hal 1.0`/`-async`/`embedded-io` traits at every
boundary, phased GPIO → timers/PWM → serial buses → analog. The bootloader never depends on it,
so the 32 KB gate (§1.2) stays safe. **Full design: `docs/plan-m3-hal.md`.**

---

## 7. Verification strategy

Per-unit gates (every firmware unit): `cargo fmt --check`, `cargo clippy -- -D warnings`,
`cargo build --release` for `thumbv7em-none-eabihf`, `cargo size` vs the 32 KB gate.

- **Host tests (`crates/boot-logic`)** — the bulk of correctness lives here (R41): CRC golden
  vector; every `image_meta` error code + check order + fallback; every protocol NAK path,
  window rewind, seq-wrap geometry at 111 blocks, HELLO-restart, power-loss ordering asserted
  via the effect sequence (metadata-erase before app-erase, metadata-write only after CRC
  match); dfu-config v1/v2/corrupt/per-ID fallback. Fault-injection: truncated/corrupt frames
  never panic or wedge the phase machine.
- **HIL smoke (probe-rs, per hardware unit):** the spike pass criteria re-run as smoke steps —
  S3 loop for U1.1 (+ software-reset FACI gate check), `candump` + `--info` for U1.2, VBTBKR
  round-trip for U0.2, tick accuracy for U0.3. Keep these scripted notes in the unit PRs, not a
  test framework — flash time is the budget (rules/testing.md).
- **End-to-end (M1/M2 exit):** SWD-flash bootloader → `can_flash.py` (unmodified, defaults
  250 k / `0x79E`/`0x79F` / 3 retries / 2 s+15 s timeouts) flashes demo app → `--info` →
  GO → boot → double-tap re-entry → re-flash → power-loss spot checks (SPEC §4.5 table).
- **Timeout policies** (host retry windows are host-side; device has none by design §4) — no
  mock-time tests needed in v1; if M2 adds device-side timeouts, test them with
  `embassy-time`'s std mock driver.
- **defmt as the diagnostic story** (replaces the C `CAN_DIAG` staircase, SPEC §9.3): stage
  logs through CAN-only entry (rail → gate → config → CAN init → serving), register snapshots
  (STR/TECR via `Debug2Format`, off the hot path) at ERROR level on bring-up failures.

---

## 8. Requirements-checklist deltas (SPEC §10 traceability)

- **Superseded by §1.2 (new values, same intent):** R1, R2 (32,768 B), R5, R12 (VTOR/MSP at
  `0x8000`; + time-driver stop), R20 (227,328), R21 (111 blocks), R36 (descriptor `16*2Ka,112*2Kg`).
- **R3 tightened:** never write below `0x8000`.
- **Deferred to M2:** R36–R39.
- **All other requirements (R4, R6–R11, R13–R19, R22–R35, R40–R43): binding as written.**

---

## 9. Open questions & assumptions

1. **App-base migration (Q):** apps must relink from `0x4000` → `0x8000`; stock Arduino IDE
   uploads to a bootloader-driven board stop working. *Proceeding on:* locked decision accepted;
   demo app + future Airform builds carry the new base; flag loudly in the README.
2. **PAC flash-register coverage (Q):** neither research doc confirms the `ra4m1` SVD exposes
   the flash-LP/FACI register set FSP's `r_flash_lp` drives. *Proceeding on:* S0 verifies; the
   fallback ladder is `ra4m1-fsp-pac`, then a contained raw-MMIO module with R01UM0007 cites.
3. **Time-driver counter (Q):** AGT vs GPT vs SysTick undecided pending S2's read of the PAC
   (which GPT channels are 32-bit on this part is unverified). *Proceeding on:* S2 picks;
   SysTick-at-1-kHz is an acceptable bootloader-grade fallback (tick resolution 1 ms is fine —
   the only fine timing, CAN bit timing, is hardware).
4. **Executor + defmt footprint in 32 KB (A):** ECO §8 has no measured bare-metal data point;
   the C bootloader was 15.6 KB with TinyUSB. *Proceeding on:* S2's ≤ 20 KB skeleton gate and
   a hard CI size gate from U0.1; defmt is first overboard if M2 pressure demands.
5. **Unverified hardware facts carried from SPEC §9.4 (A):** HOCO tolerance, VDCEN 5 ms soak
   (engineering margin), flash P/E times, VBTBKR byte count. *Proceeding on:* same values the
   bench-proven C code used.
6. **`--enter-bootloader` E2E (A):** needs an app implementing the Airform ISO-TP RPC; out of
   scope for the demo app. *Proceeding on:* verifying the identical VBTBKR[4] handoff path via
   the demo app's pin trigger satisfies the device side of R42.
7. **`embassy-usb-dfu` reuse (Q, M2):** its updater side is embassy-boot-coupled; unclear how
   cleanly the class half separates for single-slot writes. *Proceeding on:* decide in U2.2
   after reading its source; hand-rolled DFU class is the budgeted-for outcome.
8. **CAN RX polling vs ICU interrupts (A):** ticker-paced RX (§4) deviates from
   "await, don't poll." *Proceeding on:* the 4-deep-FIFO arithmetic makes 500 µs polling
   loss-proof and the ICU event-link bring-up is deferred until something needs it.
9. **Metadata format (A):** kept byte-identical per the locked decision — only the *values*
   (`image_size` ceiling) move with the new slot. No flag raised; no reason to change it found.
