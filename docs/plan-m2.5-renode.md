# Implementation Plan — M2.5: Firmware validation in the Renode emulator

> **Status — 2026-07-19: M2.5 delivered.** This document is retained as the design-of-record; the
> work shipped. The harness lives in **[`../renode/`](../renode/)** (not the planned `tools/renode/`),
> as **4 Robot suites** — `smoke`, `analog`, `serial`, `bootloader` (not the planned
> `boot`/`flash`/`can_update` names) — with **12 cases, all green on Renode v1.16.1**. Custom C#
> models delivered: `Ra4m1Gpt320`, `Ra4m1Dac`, `Ra4m1Adc`, `Ra4m1Spi`, `Ra4m1FlashLp`, `Ra4m1Can0`,
> and `MockI2CSlave` (UART/SCI uses Renode's stock model). Open-question resolutions (§8): the full
> ladder **through CAN0** was built (Q1); USB DFU stayed **out of scope** (Q3); models are kept
> **in-repo** (Q7); Renode is pinned to **v1.16.1** (Q8). CI runs the suites in the pinned
> `antmicro/renode:1.16.1` container. The honesty boundary (§2) holds: emulation validates
> register/protocol logic, not silicon — flash P/E timing and the RAM-residency fetch-stall hazard,
> CAN bit-timing, USB/DFU, real UART baud/framing, and clock tolerance stay bench-only; ICU event
> numbers and SCI2 line settings remain BENCH-VERIFY. Per-unit status is marked inline in §5.

**Contract for implementation dispatches.** This is the design plan for **Milestone 2.5**: bringing
up the RA4M1 bootloader + demo app inside the **Renode** open-source emulator
(https://github.com/renode/renode, MIT-licensed) so that boot logic, flash, and the CAN update
protocol can be exercised **deterministically, without hardware, in CI**. It is a sibling of
`docs/plan.md` (cited as **PLAN §n**) and `docs/plan-m3-hal.md` (cited as **M3 §n**) and shares
their citation conventions: behaviour context in `docs/research/master-feature-spec.md` (**SPEC §n**),
ecosystem findings in `docs/research/embassy-ra4m1-ecosystem.md` (**ECO §n**). Where this plan and
`docs/plan.md` conflict, `docs/plan.md`'s locked decisions win; this document governs only the
emulation harness and its tests.

Target under emulation: Arduino UNO R4 Minima, Renesas **R7FA4M1AB** (Cortex-M4F, 48 MHz HOCO,
256 KB code flash, 8 KB data flash, 32 KB SRAM). Triple `thumbv7em-none-eabihf`. The artifacts run
in emulation are the **release ELFs the CI `firmware-build` job already produces** (`dist/bootloader.elf`,
`dist/demo-app.elf` — `.github/workflows/ci.yml`), not an emulation-specific rebuild.

**PLAN ONLY.** No emulation code is written by this milestone's authoring step — no `.repl`, no
`.resc`, no C# peripheral models, no Robot suites. The deliverable is this document plus the
`renode-engineer` agent definition (`.claude/agents/renode-engineer.md`), reviewed and signed off
(the Open Questions in §8) before any unit dispatches.

**Renode capability sourcing.** Every Renode capability claim below carries a fetched source URL or
is marked **"unverified — confirm in spike S-Rx"**. Renode's per-target peripheral support evolves,
so no claim rests on training data. Sources were fetched against Renode **v1.16.1 (2026-02-16)** and
its docs at https://renode.readthedocs.io/en/latest/.

---

## 0. Why M2.5 exists (the framing)

M2.5 sits **between M2 (USB DFU) and M3 (application HAL)**. It exists to close a specific gap in the
verification strategy, not to add a feature.

Today the bootloader is verified at two altitudes with **nothing in between**:

- **Below — host unit tests (`crates/boot-logic`).** Pure logic (CRC, metadata codec, the CAN
  protocol state machine, DFU-config parse) is exhaustively `cargo test`-ed on the host (PLAN §7,
  rules/testing.md). This is fast and runs in CI, but by design it touches **no hardware and no
  drivers** — the protocol engine emits abstract effects (`EraseAll`, `ProgramBlock`, …) that a host
  test asserts on a mock, never against a real flash sequencer or CAN controller (PLAN §6 U0.5).
- **Above — the HIL bench (`docs/bench/m0-spikes.md` + the per-unit spike criteria).** The
  integration — boot decision, flash P/E, CAN0 register recipe, VBTBKR round-trip, the `can_flash.py`
  end-to-end flash — is proven only on the physical board, which needs the UNO R4 Minima, an
  **ESP-Prog-2 SWD probe** (`project-table/stack.md`), and a **CAN adapter** on a real bus. That bench
  is authoritative but slow, serial, manual, and impossible to run in CI on every push.

The integration layer — "do the drivers, the boot flow, and the protocol engine actually work
**together** against register-level peripherals?" — is therefore checked only by hand, only when
hardware is present. **M2.5 fills that gap with emulation:** boot the real release ELF in Renode,
model the RA4M1 peripherals the firmware drives, and assert the same behaviours the bench checklist
lists — automatically, in CI, on every change.

**Renode complements the bench; it does not replace it.** Emulation cannot reproduce real flash
program/erase timing, the "CPU cannot fetch from code flash during P/E" hazard that `flash_lp.rs`'s
RAM-residency exists to survive, USB PHY behaviour, analog, or HOCO clock tolerance (§2). Those stay
the bench's job. Renode catches the integration regressions that host tests are blind to and that
today only a manual bench run would find.

**Licensing is a non-issue** — the reason this is worth doing at all. Renode is **MIT-licensed**
(https://github.com/renode/renode). Unlike `ra4-hal` / `ra4m1-ctpac`, which were rejected for being
LGPL-3.0 (M3 §0, PLAN §1.6), nothing about depending on Renode as a **test tool** — and it is only
ever a test-time tool, never linked into shipped firmware — touches the MIT/Apache commercial
posture of this repo.

---

## 1. Feasibility assessment (the crux)

This is the make-or-break question: **can Renode model the peripherals this firmware actually
drives?** The Cortex-M4F core is the easy part; the RA4M1 peripherals are the question. The finding
below is honest and evidence-based — a real RA4M1 platform exists in Renode, but its interesting
peripherals are **unimplemented `Tag` stubs**, so the modelable subset needs custom C# work.

### The starting point: an RA4M1 platform already ships in Renode

Renode's platform tree includes **`platforms/cpus/renesas-r7fa4m1a.repl`** — the R7FA4M1 (RA4M1),
declaring `cpuType: "cortex-m4f"`, `IRQControllers.NVIC @ sysbus 0xE000E000` (SysTick handled inside
the NVIC model), 256 KB code flash as `Memory.MappedMemory @ 0x0`, and 8 KB data flash `@ 0x40100000`.
Crucially, its CAN0, USBFS, and flash-controller regions are present **only as memory `Tag`s** —
address reservations that read back nothing, not functional models
(source: https://raw.githubusercontent.com/renode/renode/master/platforms/cpus/renesas-r7fa4m1a.repl;
platform tree: https://github.com/renode/renode/tree/master/platforms/cpus). So M2.5 starts from a
real base and **extends** it, rather than describing the chip from scratch.

### Per-peripheral verdict

Peripherals enumerated from the firmware that actually drives them (`crates/ra4m1-embassy/src/*`,
`bootloader/src/drivers/*`). "OOB" = works out-of-the-box; "C#" = needs a custom `.repl` binding +
C# model; "impractical" = out of scope for M2.5.

| # | Peripheral (firmware module) | Registers driven | Renode status | Verdict | Effort | Confidence | Source / flag |
|---|---|---|---|---|---|---|---|
| 1 | **Cortex-M4F core + NVIC + SysTick + vector table** (`device.x`, `time_driver.rs` `__INTERRUPTS`) | vector table, VTOR, MSP, NVIC, `NVIC_SystemReset` | Modeled: `CortexM` + `IRQControllers.NVIC @ 0xE000E000` | **OOB** | trivial | **High** | renesas-r7fa4m1a.repl (cited above) |
| 2 | **Code + data flash as memory** (`flash_read.rs`, image reads) | code flash @ `0x0`, data flash @ `0x40100000` | Modeled: `Memory.MappedMemory` (256 KB @0x0, 8 KB @0x40100000) | **OOB** | trivial | **High** | renesas-r7fa4m1a.repl |
| 3 | **SYSTEM / CGC clocks** (`clocks.rs`) | PRCR, SCKDIVCR, SCKSCR, HOCOCR, **OSCSF (polled)**, MEMWAIT | Unknown — likely a `Tag`; firmware **spins on `OSCSF.HOCOSF`** (`clocks.rs:62`) → hangs if unmodeled | **C#** (or `.repl` register init returning HOCO-ready) | low | Med | unverified — **spike S-R1** |
| 4 | **GPT320 `embassy-time` driver + ICU/IELSR** (`time_driver.rs`) | GTWP/GTCR/GTPR/GTCNT/GTCCRA/GTST, IELSR0 (IEL0), MSTPCRD | No RA GPT/ICU model confirmed; **without it `embassy-time` never advances** — no Ticker/Timer fires, executor idles forever | **C#** (32-bit timer + compare IRQ → NVIC via ICU event-link) | med | Med | unverified — **spike S-R3** |
| 5 | **PFS / PORT pin-mux + GPIO** (`pfs.rs`, LED on P111) | PmnPFS (raw MMIO), PWPR; PORT output | PORT likely a `Tag`; Renode has generic GPIO/LED models to bind | **C#** (or generic GPIO port + LED) for observing blink | low | Med | unverified — **spike S-R3** |
| 6 | **VBTBKR backup registers** (`vbtbkr.rs`) | SYSTEM.VBTBKR byte array, PRCR.PRC1 | Unknown; the hard part is the **survive-`NVIC_SystemReset`** semantic the boot handoff depends on | **C#** (register array whose state persists across a firmware-issued soft reset) | med | Med | unverified — **spike S-R4** |
| 7 | **CAN0** (`can0.rs`) | BCR/MBM/STR mode seq, FIDCR/MKR filter, mailbox 0 TX, MB28 RX-FIFO window, MCTL/RFCR | **`Tag <0x40050000,0x40050877> "CAN0"`** — unimplemented stub. But Renode has a mature CAN framework: **CANHub** + **SocketCAN host bridge** (`CreateSocketCANBridge`, Linux-only) | **C#** (FIFO-mailbox CAN controller → CANHub) | **high** | Med | tag: renesas-r7fa4m1a.repl; CAN framework: https://renode.readthedocs.io/en/latest/host-integration/can.html — **spike S-R5** |
| 8 | **FACI-LP flash program/erase** (`flash_lp.rs`) | R_FACI_LP @ `0x407E_C000` — FPMCR/FISR/FASR/FWBL0/FCR sequencer | Only `FCACHE` present as a tag; **no P/E controller**. Modelable (write into the backing `MappedMemory`); STM32 flash-controller precedent exists in-tree | **C#** (FACI sequencer → MappedMemory) | med | Med | tag: renesas-r7fa4m1a.repl; STM32 precedent: https://renode.io/news/renode-1-14-release/ |
| 9 | **USBFS device + transceiver rail** (`usbfs.rs`, `usb_rail.rs`) | SYSCFG/DPUSR0R_FS/DCPMAXP/CFIFO/PIPECFG… ; USBMC.VDCEN, MSTPCRB | **`Tag <0x40090000,0x400900e3> "USBFS"`** — stub. Renode USB is **export-only** (host→emulation device passthrough is *not* supported); **no RA USBFS device model** | **Impractical** | very high | High (that it's infeasible) | tag: renesas-r7fa4m1a.repl; USB limits: https://renode.readthedocs.io/en/latest/tutorials/usbip.html |

### Reading of the table

- **Free (rows 1–2):** the core, NVIC, SysTick, vector-table dispatch, `NVIC_SystemReset`, and both
  flash regions as plain memory come out-of-the-box from the stock RA4M1 platform. This alone makes
  "boot the ELF and see it run" viable (M2.5.0).
- **The modelable subset (rows 3–8):** clocks-ready, the GPT320 time base, GPIO, VBTBKR
  reset-persistence, CAN0, and FACI flash are each a bounded custom C# model against a register recipe
  the firmware's own driver comments already spell out (each cites R01UM0007 sections and the C
  reference). Renode custom peripherals are first-class — implement `IDoubleWordPeripheral` /
  `BasicDoubleWordPeripheral`, use the register framework (`Read`/`Write`/`WriteOneToClear`/
  `ReadToClear` field modes), and load the `.cs` at runtime via `include @file.cs`
  (https://renode.readthedocs.io/en/latest/advanced/writing-peripherals.html). The firmware's own
  register maps are the model spec.
- **Out of scope (row 9):** USB-DFU validation in Renode is **impractical** — Renode exports emulated
  USB devices to the host but cannot bring a USB *device controller* model up for RA4M1, and there is
  no host-device passthrough. Attempting it would mean writing a full RUSB2 device model against an
  emulator that has no matching host side to enumerate it. Recommended **out of M2.5 scope** (Q3).

---

## 2. What's validatable vs. what emulation can't catch

Mapping each firmware behaviour to emulation feasibility, and stating the limits plainly.

### Validatable in Renode (with the row 3–8 models)

| Behaviour | What it needs | Mirrors bench criterion |
|---|---|---|
| **Boot to first log line** | rows 1–2 + a clocks-ready model (row 3) so `OSCSF` poll returns | S0 banner (`m0-spikes.md`) |
| **Executor + tasks run** (blink, heartbeat) | GPT320 time base (row 4) + GPIO/LED (row 5) + RTT capture (§3) | S2 1 Hz blink + `uptime` heartbeat |
| **Boot decision → mode select** | VBTBKR model with **reset persistence** (row 6) reading the `0x07738135`/`0xB007CAFE` magics (SPEC §1.4/§2) | S5 VBTBKR round-trip; PLAN §6 U1.3 |
| **`boot5` jump to app** | bootloader + demo-app both in flash; VTOR/MSP set, branch to `0x8000` (rows 1–2) | M1 exit: park → GO → app blinks |
| **Flash program/erase + read-back** | FACI model (row 8) writing into `MappedMemory` | S3 P/E loop; PLAN §6 U1.1 |
| **CAN update protocol E2E** | CAN0 model (row 7) + CANHub + SocketCAN bridge driving `can_flash.py` | M1 exit: `can_flash.py` flashes demo app incl. `--info` |

### What emulation **cannot** catch (stays the bench's job)

- **Real flash P/E timing and the fetch-stall hazard.** The entire reason `flash_lp.rs` forces its
  P/E routines RAM-resident (`#[link_section=".data"]`, no flash fetches, no `__aeabi_*` libcalls) is
  that the RA4M1 CPU **cannot fetch from code flash while the sequencer is in P/E mode**. Renode's
  MappedMemory has no such stall — a RAM-residency regression that would **brick real hardware passes
  silently in emulation**. This hazard is bench-only; call it out loudly so nobody trusts the emulator
  here.
- **CAN physical/bit-layer.** Renode CAN is **frame-level**, not bit-level (host-integration/can.html):
  the 250 kbit/s bit timing, 75 % sample point, BRP prescalers, and the SJW margin chosen for HOCO
  tolerance (`can0.rs`) are **not** exercised. The bench `candump` at 250 k owns that.
- **USB PHY / DFU.** Out of scope entirely (row 9).
- **Exact clock behaviour.** HOCO ±1–2 % tolerance and the GPT320 counter-wrap drift the bench times
  over a ≥5 min window (S2) are not meaningful under a functional (non-timing-accurate) emulator; the
  GPT320 model advances logically, not at silicon frequency.
- **Cycle-accurate delays.** The 500 ms double-tap window and the `usb_rail` 5 ms soak use
  `cortex_m::asm::delay` cycle counting (PLAN §4); Renode is **not cycle-accurate**, so these complete
  but not at wall-clock rate. Functionally fine for the boot decision; not a timing check.
- **OFS / option bytes.** The `0x400..0x43F` OFS0/OFS1/Security-MPU bytes are already byte-checked in
  CI (`ci.yml` OFS byte-check) and don't need emulation; Renode won't enforce IWDT-from-OFS either.

The honest summary: **Renode validates integration and logic-against-registers; the bench validates
physics and timing.** M2.5 is the middle rung, not a replacement for the top one.

---

## 3. Approach

### Where emulation artifacts live

A new, self-contained, firmware-untouching tree (proposed):

```
tools/renode/
  platforms/
    ra4m1-uno-r4.repl        # `using` the stock renesas-r7fa4m1a.repl; binds the custom models,
                             # overrides the CAN0/USBFS/flash tags, sets FLASH/RAM to memory.x
  models/                    # custom C# peripheral models (one file per peripheral)
    Ra4m1Cgc.cs              # SYSTEM/CGC: OSCSF.HOCOSF reads ready; PRCR key gate (row 3)
    Ra4m1Gpt320.cs           # GPT320 32-bit timer + compare IRQ → NVIC IEL0 (row 4)
    Ra4m1Vbtbkr.cs           # backup-register array; state survives NVIC_SystemReset (row 6)
    Ra4m1FaciLp.cs           # FACI-LP P/E sequencer → backing MappedMemory (row 8)
    Ra4m1Can0.cs             # FIFO-mailbox CAN0 → CANHub (row 7)
    Ra4m1Port.cs             # PFS/PORT + LED observation, if a generic model won't do (row 5)
  scripts/
    bootloader.resc          # mach create; LoadPlatformDescription; LoadELF bootloader.elf; start
    combined.resc            # bootloader + demo-app loaded together (boot5 jump path)
    can-e2e.resc             # + CANHub + CreateSocketCANBridge to vcan0
  tests/
    boot.robot               # boots to banner; asserts S0/S2 lines
    flash.robot              # FACI erase/program/read-back
    can_update.robot         # can_flash.py drives the emulated device over vcan0
```

Renode work is **contained here** and routes to the `renode-engineer` agent; only firmware-side needs
(a build variant, a defmt/feature question, an ELF-layout issue) escape to embassy-pro /
arm-cortex-expert (rules/agent-directives.md "Strict Scoping"). The models are new, MIT-licensed C#
against Renode's model API — nothing links into firmware.

### Platform `.repl` for the modelable subset

`ra4m1-uno-r4.repl` starts with `using "platforms/cpus/renesas-r7fa4m1a.repl"` (the format merges
entries; later definitions override — https://renode.readthedocs.io/en/latest/advanced/platform_description_format.html)
to inherit the core/NVIC/memory for free, then replaces the CAN0/USBFS/flash `Tag`s with the custom
models bound at their real addresses (CAN0 `@0x40050000`, FACI `@0x407EC000`), adds the CGC/GPT320/
VBTBKR/PORT models, and pins FLASH/RAM to `bootloader/memory.x` (`0x0`+32 KB, `0x2000_0000`+32 KB).

### `.resc` boot scripts

Standard sequence (https://renode.readthedocs.io/en/latest/basic/machines.html): `mach create` →
`machine LoadPlatformDescription @tools/renode/platforms/ra4m1-uno-r4.repl` → `sysbus LoadELF
@dist/bootloader.elf` (and, for the jump path, a second `LoadELF @dist/demo-app.elf` — the demo app
links at `0x8000`, disjoint from the bootloader's `0x0..0x7FFF`, so both coexist) → `start`.
`can-e2e.resc` additionally creates a CANHub, connects the CAN0 model, and bridges to host `vcan0`
via `machine CreateSocketCANBridge` (Linux-only — host-integration/can.html).

### Firmware output capture (for assertions)

The firmware logs via **defmt over defmt-RTT** (SEGGER RTT) — PLAN §2/§7. Renode **supports RTT**:
it ships a `renesas-segger-rtt.py` helper and an RTT console demo used by `scripts/single-node/
ek-ra2e1.resc`, and RTT surfaces through the UART capture path so Robot's `Wait For Line On Uart`
keywords read it (https://renode.readthedocs.io/en/latest/host-integration/uart.html;
https://renode.readthedocs.io/en/latest/introduction/testing.html). **Caveat (flagged):** the exact
**defmt** wire framing over RTT under Renode was inferred from the RTT+UART path, not a defmt-specific
doc — **confirm in spike S-R2**. Fallbacks if defmt framing needs host-side decode: (a) a small
defmt-decoder step over a captured RTT file backend (`uart CreateFileBackend @out.log`), or (b) build
the emulation ELF with a plain-text UART/`semihosting` log sink so assertions read human-readable
lines directly. The S0/S2 banners (`m0-spikes.md`) are the concrete lines to assert.

### Robot Framework test-suite structure

Renode's primary test path is Robot via `renode-test my_test.robot`
(https://renode.readthedocs.io/en/latest/introduction/testing.html). Each suite: set up the machine
with `Execute Command` (or `Create Machine` from a `.resc`), `Start Emulation`, then assert on output
with `Wait For Line On Uart` / `Wait For Next Line On Uart` and drive input with `Write Line To Uart`
/ raw `Execute Command`. One independent case per behaviour from the §2 "validatable" table; each case
names the bench criterion it mirrors and what it does **not** cover.

---

## 4. CI integration

Emulation tests join the existing GitHub Actions pipeline (`.github/workflows/ci.yml`) as a **new job
gated behind `firmware-build`** (it consumes that job's `dist/*.elf` artifacts — no rebuild).

- **Runner:** `ubuntu-latest` (matches the existing jobs; also required — the SocketCAN bridge is
  **Linux-only**, host-integration/can.html).
- **Renode provisioning:** the **official action `antmicro/renode-test-action@v5`**
  (https://github.com/antmicro/renode-test-action; blog:
  https://antmicro.com/blog/2024/10/renode-github-action-for-automated-testing-in-simulation), or the
  pinned `antmicro/renode:1.16.1` Docker image (https://hub.docker.com/r/antmicro/renode) — pin the
  version so platform behaviour is reproducible. Headless is native: `--console --disable-gui`,
  `-e "<cmd>"`, `-P <port>` (https://github.com/renode/renode/blob/master/README.md).
- **What runs, staged with the milestones:** `boot.robot` (boot + blink/heartbeat) and `flash.robot`
  gate first; `can_update.robot` (needs `modprobe vcan` + a `vcan0` interface on the runner) gates
  once M2.5.3 lands. Each Robot run carries a **hard timeout** so a wedged machine fails the job
  instead of hanging it.
- **What a passing emulation test gates:** integration regressions between `boot-logic`, the drivers,
  and the boot flow that the host tests structurally cannot see — without a bench. It sits **between**
  the `host-tests` job (below) and the manual bench (above); it does **not** replace the size/OFS/
  clippy gates, and a green emulation run is explicitly **not** a substitute for the M1/M2 bench exit
  criteria (§2 limits).

---

## 5. Milestones & implementation units

**Shared-file discipline (mirrors PLAN §6):** **M2.5.0** creates the `tools/renode/` tree and every
stub file (`.repl`, `.resc`, empty model files, the Robot suite skeleton) so later units fill only
their own claimed files. CI wiring (`ci.yml`) is touched once, in M2.5.4.

**Agent routing:** all Renode artifacts (`.repl`, `.resc`, C# models, Robot suites, CI wiring) →
**`renode-engineer`**. Any firmware-side tweak an emulation need surfaces (a build variant, a defmt/
feature flag, an ELF-layout question) → **embassy-pro** (executor/time/driver seams) or
**arm-cortex-expert** (register/vector-table/boot-flow). USB is not dispatched (Q3).

### M2.5.0 — Minimal machine boots the ELF ✅ Delivered

| Unit | Description | Files | Routing |
|---|---|---|---|
| **U2.5.0** | `tools/renode/` scaffold; `ra4m1-uno-r4.repl` = `using` the stock `renesas-r7fa4m1a.repl` + FLASH/RAM from `memory.x`; `bootloader.resc` loads `dist/bootloader.elf` and starts; add the **CGC-ready** model (row 3) so the `OSCSF.HOCOSF` poll returns. Acceptance = **spike S-R1** (boots to the first RTT/defmt or UART line). | `tools/renode/**` (scaffold), `platforms/ra4m1-uno-r4.repl`, `scripts/bootloader.resc`, `models/Ra4m1Cgc.cs` | renode-engineer |

### M2.5.1 — `.repl` for the modelable RA4M1 peripherals ✅ Delivered

| Unit | Description | Files | Routing |
|---|---|---|---|
| **U2.5.1a** | **GPT320 time-base model** (row 4) + ICU/IELSR→NVIC IEL0 wiring so `embassy-time` advances; **PORT/GPIO + LED** (row 5). Acceptance = **spike S-R3** (executor ticks; blink + `uptime` heartbeat lines appear, mirroring S2). | `models/Ra4m1Gpt320.cs`, `models/Ra4m1Port.cs`, `platforms/ra4m1-uno-r4.repl` | renode-engineer |
| **U2.5.1b** | **VBTBKR model** with soft-reset persistence (row 6); `combined.resc` loads bootloader + demo-app; assert **boot decision** (magic → mode) and the **`boot5` jump** (park → GO → demo-app blinks). Acceptance = **spike S-R4** + the jump path. | `models/Ra4m1Vbtbkr.cs`, `scripts/combined.resc` | renode-engineer |

### M2.5.2 — FACI flash model: erase/program works ✅ Delivered

| Unit | Description | Files | Routing |
|---|---|---|---|
| **U2.5.2** | **FACI-LP model** (row 8): the P/E command sequence writes into the backing code/data-flash `MappedMemory`, read-back verifies. Mirrors S3 (erase → program → read-back a scratch block). **Documents that RAM-residency is NOT tested here** (§2 limit). | `models/Ra4m1FaciLp.cs`, `tests/flash.robot` | renode-engineer |

### M2.5.3 — CAN bridge: `can_flash.py` E2E in emulation ✅ Delivered (partial)

> **Delivered as frame-level.** `Ra4m1Can0.cs` bridges to the stock `CANHub` and `bootloader.robot`
> asserts a HELLO → reply exchange via the native `CANTester`. The full `can_flash.py` end-to-end
> loop still needs `CreateSocketCANBridge` (Linux-only) and is **not** wired into CI — see
> [`../renode/TODO.md`](../renode/TODO.md).

| Unit | Description | Files | Routing |
|---|---|---|---|
| **U2.5.3** | **CAN0 model** (row 7, FIFO-mailbox mode) → CANHub → `CreateSocketCANBridge` to host `vcan0`; drive the emulated device with the **`main:tools/can-flash/can_flash.py`** update flow (HELLO/`--info`, ERASE, windowed program, GO). Assess whether `can_flash.py` speaks a SocketCAN backend unmodified (Q4). Acceptance = **spike S-R5** then a full demo-app flash. | `models/Ra4m1Can0.cs`, `scripts/can-e2e.resc`, `tests/can_update.robot` | renode-engineer (+ embassy-pro if a firmware/`can_flash.py` seam needs review) |

### M2.5.4 — Robot suite + CI ✅ Delivered

| Unit | Description | Files | Routing |
|---|---|---|---|
| **U2.5.4** | Consolidate `boot.robot` (S0/S2 assertions) + `flash.robot` + `can_update.robot`; wire a headless Renode job into `ci.yml` behind `firmware-build` (via `antmicro/renode-test-action@v5` or pinned Docker), `modprobe vcan`, hard per-suite timeout. Acceptance: the suite passes headless in CI. | `tests/*.robot`, `.github/workflows/ci.yml` | renode-engineer |

### Stretch (assessed, not committed) — USB DFU

Row 9 is **impractical** in Renode (export-only USB, no RA USBFS device model). Named here for
completeness and **recommended out of scope** (Q3); revisit only if Renode gains an RA USB device
model upstream.

**Order:** U2.5.0 → U2.5.1a → U2.5.1b → U2.5.2 → U2.5.3 → U2.5.4, each gated by its spike. U2.5.2 and
the U2.5.1 pair are independent once U2.5.0 boots and can parallelize if two agents are available.

---

## 6. De-risking spikes (ordered; each retires one make-or-break unknown)

Each spike lands as a small commit under `tools/renode/` and reports pass/fail **before** the unit
that builds on it dispatches (PLAN §5 discipline). These are the early checks that decide whether M2.5
is cheap, expensive, or infeasible.

- **S-R1 — Does Renode boot our actual ELF at all?** Load `dist/bootloader.elf` on the stock
  `renesas-r7fa4m1a.repl` (+ CGC-ready shim) and reach the **first RTT/defmt or UART line**. Retires,
  in one shot: core/memory/vector-table/`LoadELF` viability **and** the `OSCSF.HOCOSF` spin risk
  (row 3). **Pass:** a log line appears; the machine does not wedge in the clock poll. **Fail →** the
  CGC model must return HOCO-ready (small); if the vector table or `LoadELF` misbehaves, re-examine
  the `memory.x`↔`.repl` map. *This is the single most important gate — if the real ELF won't boot,
  M2.5 stops here.*
- **S-R2 — Can we capture firmware output for assertions?** Confirm **defmt-over-RTT** is readable by
  Renode's RTT/UART keywords, or fall back to an RTT file backend + host defmt-decode, or a plain-text
  UART log build. **Pass:** the S0 banner text is asserted by `Wait For Line On Uart`. (The research
  flagged defmt framing as inferred, not doc-confirmed — this spike removes that doubt.)
- **S-R3 — Does the executor tick?** Bring up the GPT320 time-base model so `embassy-time` advances
  and a Ticker fires. **Pass:** the S2 1 Hz blink + `uptime` heartbeat lines appear. **Fail →** the
  time model is the crux of M2.5.1; a SysTick-based shim is a fallback only if the firmware could be
  built to use it (firmware-side — embassy-pro), otherwise GPT320 must be modeled.
- **S-R4 — Does backup-register state survive a firmware `NVIC_SystemReset`?** The boot-mode handoff
  (SPEC §1.4/§2) depends on VBTBKR surviving a soft reset. Prove the VBTBKR model keeps its bytes
  across the reset the firmware issues, and that the boot decision then selects the right mode.
  **Pass:** magic written → soft reset → boot decision enters the expected mode; word reads cleared
  after consumption (mirrors S5).
- **S-R5 — Does the CAN host-bridge reach `can_flash.py`?** Stand up the CAN0 model + CANHub +
  SocketCAN bridge on `vcan0` and confirm `main:tools/can-flash/can_flash.py --info` gets a reply
  from the emulated device. **Pass:** `--info` round-trips; a single block programs. **Fail / Q4:** if
  `can_flash.py` has no SocketCAN backend, decide between a thin backend shim vs. a Renode-specific
  python-can harness that drives the same protocol. *This gate decides whether E2E-in-emulation is
  real or whether M2.5 stops at flash-level (M2.5.2).*

Order rationale: S-R1 proves the foundation; S-R2 makes any assertion possible; S-R3/S-R4 unlock the
boot+executor behaviours; S-R5 is the ambitious end-to-end check and is deliberately last.

---

## 7. Reuse ledger — what M2.5 leverages

M2.5 is not greenfield; it stands on artifacts M0–M2 already produced.

| Existing asset | Where | How M2.5 uses it |
|---|---|---|
| **Release ELFs/bins** | CI `firmware-build` → `dist/bootloader.elf`, `dist/demo-app.elf` (`ci.yml`) | Loaded verbatim via `sysbus LoadELF` — the emulator runs the *shipped* image, not a rebuild. |
| **`can_flash.py`** | `main:tools/can-flash/can_flash.py` | The E2E driver for M2.5.3 — the same unmodified host tool the bench uses (subject to Q4). |
| **`boot-logic` host tests** | `crates/boot-logic` (PLAN §7) | The layer **below** M2.5: pure logic is already exhaustively tested, so emulation targets integration, not re-testing the codecs/state machine. |
| **Bench checklists** | `docs/bench/m0-spikes.md` + per-unit spike criteria | The **behaviours to mirror**: S0 banner → `boot.robot`; S2 blink/heartbeat → `boot.robot`; S3 P/E loop → `flash.robot`; S5 VBTBKR → S-R4; M1 `can_flash.py` E2E → `can_update.robot`. |
| **Firmware driver register maps + R01UM0007 cites** | `bootloader/src/drivers/*`, `crates/ra4m1-embassy/src/*` | The **spec for the C# models** — each driver already documents the exact registers, bit semantics (W1C/W0C), and mode sequences the model must reproduce. |
| **Stock RA4M1 platform** | Renode `platforms/cpus/renesas-r7fa4m1a.repl` | The base the custom `.repl` `using`-extends — core/NVIC/SysTick/flash memory for free. |
| **`memory.x` maps** | `bootloader/memory.x`, `demo-app/memory.x` | Single source of truth for the FLASH/RAM regions the `.repl` pins (`0x0`+32 KB, `0x8000` app base, `0x2000_0000`+32 KB). |
| **Official Renode CI action / Docker** | `antmicro/renode-test-action@v5`, `antmicro/renode:1.16.1` | Headless provisioning in `ci.yml` — no bespoke install. |

---

## 8. Open questions & assumptions

For the user to sign off before any unit dispatches.

1. **How much custom C# is worth writing (Q).** The modelable subset (CGC, GPT320, VBTBKR, PORT,
   FACI, CAN0) is **six C# peripheral models** — real, if bounded, work. *Proposal:* stage it by value
   — M2.5.0–1 (boot + executor + boot decision) delivers most of the integration coverage cheaply;
   FACI (M2.5.2) and CAN0 (M2.5.3) are progressively more effort. **Sign-off: commit to the full
   ladder through CAN0, or stop after the flash level (M2.5.2) and leave CAN E2E to the bench?**
2. **Accepting partial emulation (A).** Emulation deliberately cannot catch flash P/E timing/RAM-
   residency, CAN bit-timing, USB, or clock tolerance (§2). *Proceeding on:* the bench remains
   authoritative for those; a green emulation run never substitutes for the M1/M2 bench exit criteria.
   Flagged so no one over-trusts the emulator (especially the RAM-residency hazard).
3. **USB DFU in scope? (Q).** Row 9 is impractical (Renode USB is export-only; no RA USBFS device
   model). *Proposal:* **out of scope for M2.5**; revisit only if Renode gains an RA USB device model
   upstream. **Sign-off: agree USB is out?**
4. **Unmodified `can_flash.py` vs. a Renode-specific harness (Q, → S-R5).** E2E depends on
   `can_flash.py` talking to `vcan0` over a SocketCAN backend. *Proceeding on:* S-R5 checks whether
   `can_flash.py` supports a SocketCAN backend as-is; if not, choose a thin backend shim (keeps the
   *exact* host tool in the loop) over a reimplemented python-can harness (drifts from what the bench
   runs). **Sign-off after S-R5.**
5. **Linux-only CAN bridge (A).** `CreateSocketCANBridge` is Linux-only; CI is already `ubuntu-latest`
   and the runner can `modprobe vcan`. *Proceeding on:* no cross-platform emulation-CAN need; local
   dev on non-Linux uses the flash-level suites and the bench for CAN.
6. **defmt-over-RTT framing under Renode (A, → S-R2).** Confirmed transport path (RTT + UART capture),
   but exact defmt framing was inferred, not doc-confirmed. *Proceeding on:* S-R2 confirms; fallbacks
   are an RTT file backend + host defmt-decode, or a plain-text UART log build for emulation.
7. **Upstream the RA4M1 models? (Q).** The CAN0/FACI/GPT320 models could be contributed back to
   Renode (they'd benefit any RA4M1 user) or kept in `tools/renode/`. *Proposal:* keep in-repo for
   M2.5; consider upstreaming later. **Sign-off: in-repo only, or plan to upstream?**
8. **Renode version pin (A).** Fetched against v1.16.1. *Proceeding on:* pin the action/Docker to a
   specific release so platform behaviour is reproducible; bump deliberately, like a toolchain pin.
9. **Board vs. CPU platform (A).** Only the RA4M1 **CPU** `.repl` is confirmed in-tree; an EK-RA4M1
   **board** file was not found in the supported-boards doc. *Proceeding on:* the CPU platform is the
   right base anyway — this is a custom UNO R4 Minima board described in `ra4m1-uno-r4.repl`, not an
   EK-RA4M1.
```
