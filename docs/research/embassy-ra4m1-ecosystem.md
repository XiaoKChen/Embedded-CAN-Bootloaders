# Rust/Embassy ecosystem readiness — RA4M1 (Arduino Uno R4 Minima) CAN bootloader

Research date: 2026-07-16. Goal: assess rewriting the existing C bootloader
(CAN flashing, image validation, app jump — 16 KB slot at 0x0000_0000) in Rust,
preferably on Embassy. Every claim carries a source URL and a confidence level.

---

## 1. Embassy RA HAL status

**Finding: there is NO official Embassy-maintained RA HAL.** The Embassy README
lists its maintained HALs as `embassy-stm32`, `embassy-nrf`, `embassy-rp`,
`embassy-mspm0`, and `embassy-mcxa`. Renesas appears only in the
community/external HALs section, as:

> "[ra-hal](https://git.sr.ht/~az1/ra-hal), for the Renesas RA family of microcontrollers."

So "we will be using the ra-hal" refers to a **community sourcehut project by
`~az1`**, published on crates.io as **`ra4-hal`** (project umbrella:
`https://sr.ht/~az1/ra4-rs/`). What it actually is:

- **Scope:** RA4M1 only (48/64/100-pin variants; 64-pin default — the Uno R4's
  R7FA4M1AB3CFM is the 64-pin part).
- **Peripherals implemented:** GPIO (PORT), UART (SCI), SPI, I2C (IIC), 14-bit
  ADC, 12-bit DAC, GPT timers/PWM, DTC, CRC, WDT, RTC (experimental).
- **Peripherals NOT implemented:** **CAN, USB** (embassy-usb-driver is a
  dependency but unimplemented), **and flash programming** — i.e. the two
  peripherals this bootloader lives on (CAN + flash) are both missing.
- **Embassy integration:** genuine — embassy-time driver, `bind_interrupts!`
  macro, async `embedded-hal-async` traits alongside blocking ones.
- **Maturity:** very young and very small. crates.io: v0.2.3, crate created
  **2026-02-14**, last updated **2026-03-10**, ~110 total downloads,
  **LGPL-3.0-or-later**, MSRV 1.91, single maintainer. Built on the
  `ra4m1-ctpac` PAC (same author).

Evidence:
- https://github.com/embassy-rs/embassy/blob/main/README.md (maintained-HAL list + ra-hal community listing)
- https://docs.rs/ra4-hal (module/peripheral inventory)
- https://crates.io/api/v1/crates/ra4-hal (dates, version, downloads, license)
- Note: `sr.ht/~az1/ra4-rs` and `git.sr.ht/~az1/ra-hal` returned HTTP 502 on
  every fetch attempt (3 tries on 2026-07-16); repo-level activity could not be
  inspected directly. Facts above come from crates.io metadata, docs.rs, and
  the Embassy README.

Other community HALs (none Embassy-based, none with CAN or flash):
- `arduino-uno-r4-hal` v0.1.0 (kanade-k-1228, single release 2025-08-19,
  embedded-hal blocking) — https://crates.io/api/v1/crates/arduino-uno-r4-hal,
  https://github.com/kanade-k-1228/arduino-uno-r4-hal
- `Kevin-Konradi/r7fa4m1ab-hal` (GitHub only, last touched 2025-10-24) —
  https://github.com/Kevin-Konradi/r7fa4m1ab-hal
- `dw-labs-org/UNO-R4-rust` (Dom Wilson — see §5 and §7; this one DOES have CAN).

**Confidence: high** (Embassy README checked directly; crates.io API checked
directly). Caveat: sourcehut outage prevented reading the HAL source itself.

---

## 2. PAC availability

**Finding: yes, multiple usable PACs exist for RA4M1.** Three options:

| Crate | Generator | Source SVD | Latest | Updated | License |
|---|---|---|---|---|---|
| `ra4m1` | svd2rust v0.28 | Keil CMSIS packs (via ra-rs/ra) | 0.2.0 | 2022-12-28 (stale) | MIT OR Apache-2.0 |
| `ra4m1-fsp-pac` | svd2rust | Keil CMSIS packs (via ra-rs/ra) | 0.2.0 | 2026-01-10 | MIT OR Apache-2.0 |
| `ra4m1-ctpac` | chiptool (embassy-style) | Keil SVD | 0.2.3 | 2026-03-10 | LGPL-3.0-or-later |

- The `ra-rs/ra` GitHub org hosts svd2rust PACs for 29 RA devices including
  RA4M1; SVDs come from ARM's Keil CMSIS-Pack repository. Repo activity is
  minimal (12 commits, no GitHub releases) but the generated output is proven:
  Dom Wilson's Uno R4 work (UART, CAN, clocks, interrupts) is built on the
  `ra4m1` crate.
- `ra4m1-ctpac` is the chiptool-generated PAC underlying `ra4-hal`; note the
  LGPL license (see recommendations).
- There is **no embassy-metapac for RA** (metapac exists only for STM32-style
  Embassy-maintained HALs).

Evidence:
- https://github.com/ra-rs/ra
- https://crates.io/api/v1/crates/ra4m1/versions, https://crates.io/api/v1/crates/ra4m1-fsp-pac/versions (licenses/dates)
- https://crates.io/api/v1/crates/ra4m1-ctpac
- https://domwil.co.uk/posts/uno-r4-rust/ (real-world use of the `ra4m1` PAC)

**Confidence: high.**

---

## 3. embassy-boot

**Finding: architecture understood; it is chip-agnostic and usable without an
official Embassy HAL — but its A/B swap model conflicts with this project's
single-slot 256 KB flash layout.**

Architecture (from the Embassy docs/book):
- Four page-aligned partitions: **BOOTLOADER** (~8 KB, up to 24 KB for
  debugging), **ACTIVE** (app runs from here), **DFU** (staging; must be at
  least one page **larger** than ACTIVE), **BOOTLOADER STATE** (swap progress).
- Power-fail-safe **swap algorithm** with trial boot and rollback, driven by
  the STATE partition.
- Flash access goes through **`embedded-storage` NorFlash traits** — "works
  with any flash implementing embedded_storage". The bootloader decision path
  is **blocking**; async exists only in the application-side
  `FirmwareUpdater` (blocking variants `BlockingFirmwareUpdater` /
  `BlockingFirmwareState` also provided). The bootloader does NOT run an
  Embassy executor.
- **No transport in the bootloader.** embassy-boot expects the *application*
  to receive the new image (over CAN, in our case) and write it into DFU; the
  bootloader only verifies/swaps on next boot. Prebuilt chip crates exist for
  nRF/RP/STM32 only, but they are thin wrappers — the core crate + a custom
  NorFlash impl is the supported path for other chips.

**Fit against this project (verified against the current C layout in git
history, `src/boot/memory_map.h` @ commit 43d3de2):** the existing design is
single-slot — 16 KB bootloader + 238 KB app slot + 2 KB metadata block in
256 KB code flash, with the CAN download performed *by the bootloader*.
Adopting embassy-boot means:
1. ACTIVE + DFU partitioning → app capacity drops to roughly **~117 KB**
   (DFU ≥ ACTIVE + one 2 KB page, plus STATE), i.e. **the usable app slot is
   halved**.
2. The update model inverts: the *application* must carry the CAN download
   path (or the bootloader must be extended with a custom recovery transport,
   which is off embassy-boot's paved road).
3. A custom `NorFlash` driver for the RA4M1 flash-LP controller must be
   written regardless (§4) — embassy-boot removes none of that work.

What it buys: battle-tested power-fail-safe swap + automatic rollback, which
the current C bootloader does not have (its fail-safety is "metadata block
written last").

**Alternative — plain bare-metal Rust bootloader (cortex-m-rt, no
embassy-boot):** a direct port of the existing logic (polled CAN receive →
erase/program app slot → CRC32 validate → write metadata last → set VTOR →
jump). Same flash driver work, same CAN driver work, no partition redesign, no
host-flasher protocol change, keeps the full 238 KB app slot. Loses nothing
the current C code has; gains none of embassy-boot's rollback.

Evidence:
- https://docs.embassy.dev/embassy-boot/git/default/index.html
- https://embassy-rs.github.io/embassy-book/embassy/dev/bootloader.html
- https://github.com/embassy-rs/embassy/tree/main/embassy-boot
- https://blog.drogue.io/firmware-updates-part-1/ (design rationale, size)
- Local: `git show 43d3de2:src/boot/memory_map.h` (existing partition layout)

**Confidence: high** on embassy-boot's architecture; **high** on the layout
conflict (arithmetic on verified constants).

---

## 4. Flash driver (code-flash programming on RA4M1)

**Finding: no existing Rust driver for the RA4M1 flash-LP controller was
found. It must be written from the PAC.** Searched: crates.io keywords
(renesas, ra4m1), docs.rs of `ra4-hal` (explicitly lacks flash), GitHub repo
search for `ra4m1`, and the ra-rs org. Zephyr did not have flash-LP support
for RA4M1 upstream either as of the XIAO RA4M1 board PR (#87782), confirming
this is a genuinely thin area even outside Rust.

Hardware constraints (FSP `r_flash_lp` documentation — the RA4M1 uses the
**flash-LP** peripheral, not the flash-HP FCU of larger RA parts):
- While programming/erasing **code flash, all supporting code must reside in
  RAM** — the CPU cannot fetch from code flash in P/E mode. Renesas ships an
  FSP option to relocate the whole driver to RAM.
- Interrupts must be disabled (vector-table fetches from code flash during P/E
  mode error out).
- Geometry (verified against the existing C `memory_map.h`, which mirrors the
  FSP values): code flash 256 KB, **2 KB erase blocks, 8-byte write unit**;
  data flash 8 KB at 0x4010_0000, 1 KB erase blocks.
- The bootloader's own 16 KB region (including OFS0/OFS1 option bytes at
  0x400/0x404) is never self-written — the Rust port must preserve that
  invariant and the option-byte placement in its linker script.

Rust handling of the RAM-execution constraint is a known pattern:
`#[link_section = ".data"]` (or a dedicated `.ramfunc` section copied by the
cortex-m-rt init, plus `#[inline(never)]`) so the program/erase inner routines
execute from SRAM. No RA4M1-specific example was found; the pattern itself is
standard cortex-m practice.

Evidence:
- https://renesas.github.io/fsp/group___f_l_a_s_h___l_p.html (RAM-residency, interrupt constraints)
- https://en-support.renesas.com/knowledgeBase/20037102 (running FSP flash driver from RAM)
- https://github.com/zephyrproject-rtos/zephyr/pull/87782 (flash-lp not upstream for RA4M1)
- https://docs.rs/ra4-hal (no flash module)
- Local: `git show 43d3de2:src/boot/memory_map.h` (geometry constants)

**Confidence: high** that no ready-made driver exists (multiple search
avenues); **high** on the constraints (vendor docs).

---

## 5. CAN (classic R_CAN peripheral)

**Finding: one real Rust driver for the RA4M1 CAN0 peripheral exists — Dom
Wilson's `dw-labs-org/UNO-R4-rust` — but it is a template repo, not a
published crate.** Details (read from the source):

- `src/can.rs` (~27.6 KB source): mailbox-based driver for `ra4m1::CAN0`
  (classic R_CAN, 32 mailboxes), bit-timing config, interrupt-driven TX/RX
  handlers, mailbox search via MSMR/MSSR, and it **implements
  `embedded_can::Frame`**. RTIC example included. **MIT licensed**, built on
  the `ra4m1` svd2rust PAC. Repo last updated 2026-02-15.
- The author's blog part 3 ("all about CAN") was not yet published as of
  2026-07-16 — the code exists but is undocumented.
- `ra4-hal` has **no CAN support**. No CAN crate for RA on crates.io was found
  (searched: crates.io keyword "renesas", "ra4m1 can", GitHub code/repo search).
- Trait to target: **`embedded-can`** (Id/Frame/nb & blocking traits) — this
  is the ecosystem-standard abstraction and what Dom Wilson's driver uses.

**Effort estimate for a bootloader-grade driver:** modest. The existing C
bootloader already contains a *polled* mailbox driver (`src/drivers/can_polled.c`
in git history) with the exact register sequence; porting that against any of
the three PACs, with Dom Wilson's MIT code as a cross-reference, is a
contained, well-understood task (no interrupts needed in a bootloader).

Evidence:
- https://github.com/dw-labs-org/UNO-R4-rust (src/can.rs verified via GitHub API; license MIT via API)
- https://domwil.co.uk/posts/uno-r4-rust_2/ (CAN promised for part 3)
- https://crates.io/crates/embedded-can
- https://renesas.github.io/fsp/group___c_a_n.html (peripheral/mailbox model)

**Confidence: high.**

---

## 6. Tooling (probe-rs, defmt/RTT, SWD access)

**Finding: fully workable.**

- **probe-rs:** the `R7FA4M1AB` target including a flash algorithm was added
  in probe-rs v0.21.0 (PR #1706, merged 2023-07-30). Verified present in
  current master: `probe-rs/targets/RA4M1_Series.yaml` lists `R7FA4M1AB`
  (checked via GitHub code search 2026-07-16). So `probe-rs run` /
  `cargo flash --chip R7FA4M1AB` works, which also solves flashing the
  bootloader itself at 0x0.
- **defmt/RTT:** nothing chip-specific — standard defmt-rtt + probe-rs over
  SWD applies. (No RA4M1-specific defmt issue found; low risk.)
- **SWD on the Uno R4 Minima:** confirmed — the board ships with a
  **presoldered 10-pin Cortex debug (SWD) connector**; SWDIO is P108, SWCLK is
  P300. Arduino's own debugger tutorial and SEGGER's knowledge base both
  document it.
- **Caution flag:** writing to 0x0 replaces the Arduino bootloader; Dom Wilson
  bricked a board using Renesas `rfp-cli` over the ROM boot mode and needed an
  E2-Lite to recover. With SWD + probe-rs this is recoverable, but a debug
  probe should be considered mandatory equipment for this project.

Evidence:
- https://github.com/probe-rs/probe-rs/pull/1706 (merged 2023-07-30)
- https://github.com/probe-rs/probe-rs/discussions/1800 (v0.21.0 release notes)
- probe-rs master: `probe-rs/targets/RA4M1_Series.yaml` (GitHub code search)
- https://docs.arduino.cc/tutorials/uno-r4-minima/debugger/
- https://kb.segger.com/Arduino_Uno_R4_Minima (SWDIO P108 / SWCLK P300)
- https://domwil.co.uk/posts/uno-r4-rust/ (rfp-cli bricking anecdote)

**Confidence: high.**

---

## 7. Prior art

**Finding: real but thin. Rust definitely runs on the Uno R4; nobody was found
running embassy-boot (or any Rust bootloader) on RA4M1.**

- **Dom Wilson's series** — the deepest work found: clock setup to 48 MHz,
  interrupt infrastructure, generic SCI UART with embedded-io traits, and the
  CAN driver of §5; runs under the stock Arduino bootloader at 0x4000.
  https://domwil.co.uk/posts/uno-r4-rust/ , https://domwil.co.uk/posts/uno-r4-rust_2/ ,
  https://github.com/dw-labs-org/UNO-R4-rust
- **`ra4-hal` / ra4-rs** (§1) — the Embassy-README-endorsed community HAL;
  proves Embassy executor + time driver run on RA4M1.
- **`arduino-uno-r4-hal`** — blocking embedded-hal HAL, ~10k downloads.
  https://github.com/kanade-k-1228/arduino-uno-r4-hal
- Arduino forum thread confirming community interest:
  https://forum.arduino.cc/t/rust-and-arduino-uno-r4/1415224
- **embassy-boot on non-official-HAL chips:** the design supports it (any
  `embedded-storage` NorFlash, §3), but **no published RA example was found**
  after searching GitHub, crates.io, and the Embassy repo/issues. Embassy's
  issue tracker has no Renesas RA support issue at all (gh issue search,
  2026-07-16).

**Confidence: medium-high** — absence-of-evidence claims are only as good as
the searches (GitHub repo/code search, crates.io, Embassy issues, web search).

---

## 8. Size budget

**Finding: the budget that matters is 16 KB (0x0000_0000–0x0000_3FFF).**
Verified twice: the current C bootloader's `memory_map.h` (`BOOT_SIZE
0x4000`, "16 KB, 8 blocks") and the stock Arduino bootloader's identical
16 KB reservation (app at 0x4000, per Dom Wilson's write-up). Keeping 0x4000
preserves the app-side linker scripts and the host flasher contract.

Footprint data found:
- **embassy-boot: ~8 KB flash** in release configuration; Embassy's book
  recommends **24 KB when debugging** it (debug builds/logging inflate it past
  16 KB — plan for release-only fit, or temporarily steal app blocks during
  bring-up). https://embassy-rs.github.io/embassy-book/embassy/dev/bootloader.html
- No published figure for a bare-metal Rust bootloader on RA4M1 exists. The
  8 KB embassy-boot number (which includes the swap machinery) is the best
  available proxy; a bare-metal port with polled CAN + flash-LP + CRC32 and
  `opt-level = "z"` + LTO + `panic-immediate-abort`-style settings has no
  identified reason to exceed 16 KB, but this is an **estimate, not data** —
  put a `cargo size` check in CI from day one.

**Confidence: high** on the 16 KB budget and the embassy-boot figure;
**low-medium** on the bare-metal projection (no measured data point found).

---

## Recommendation

Headline: **the ecosystem is ready enough for a bare-metal Rust bootloader,
and NOT ready for an "everything on ra-hal/Embassy" bootloader.** The two
peripherals the bootloader depends on — flash-LP and CAN — are exactly the two
things no HAL provides; both must be written against a PAC regardless of which
architecture is chosen. Embassy's value lands in the *application*, not here.

### Option A — Bare-metal Rust bootloader (cortex-m-rt + PAC), Embassy for the app — RECOMMENDED
Direct port of the existing, working C design: polled CAN mailbox driver +
flash-LP driver (RAM-resident program/erase) + CRC32 validation + metadata-
block-last commit + VTOR/jump. Use the MIT `ra4m1` or `ra4m1-fsp-pac` PAC
(avoids the LGPL question of `ra4m1-ctpac`).
- **Pros:** preserves the proven 16 KB / 238 KB single-slot layout, the host
  flasher protocol, and the fail-safety model; no executor/time-driver
  footprint; every hard problem (flash, CAN) is identical across all options
  anyway; Dom Wilson's MIT CAN driver de-risks the register work.
- **Cons:** no trial-boot/rollback beyond what the C design had; you own the
  boot flow (vector table, OFS0/OFS1 option-byte placement in the linker
  script) yourself.
- **Biggest risks:** the flash-LP driver (RAM-execution discipline, interrupt
  masking) — the one component with zero Rust prior art; and linker-script
  correctness for the option bytes at 0x400/0x404.

### Option B — embassy-boot core + custom NorFlash driver
Write the same flash-LP driver but expose it as `embedded-storage::NorFlash`;
let embassy-boot own partitions/swap/rollback; move the CAN download into the
Embassy application (`FirmwareUpdater` writes DFU).
- **Pros:** power-fail-safe swap and automatic rollback, maintained upstream;
  the app-side updater is async and fits an Embassy app naturally; ~8 KB
  bootloader fits the 16 KB slot.
- **Cons:** **halves the app slot to ~117 KB** (ACTIVE + DFU + STATE in
  256 KB); inverts the update model — a device with a broken app can no longer
  be recovered over CAN unless you additionally bolt a custom CAN transport
  into the bootloader (off embassy-boot's paved road); host flasher protocol
  and partition map both change.
- **Biggest risks/unknowns:** no known prior art of embassy-boot on any RA
  chip; recovery-path story for a bricked application; debug builds exceeding
  16 KB during bring-up.

### Option C — Full Embassy bootloader on ra4-hal — NOT recommended today
`ra4-hal` + embassy-executor inside the bootloader.
- **Pros:** one HAL across bootloader and app; async niceties.
- **Cons:** ra4-hal has **no CAN and no flash driver** (you still write both),
  is months old with a single maintainer and ~110 downloads, and is
  **LGPL-3.0** (statically-linked firmware compliance burden); executor + time
  driver spend flash budget for no bootloader benefit.
- **Biggest risk:** betting the recovery-critical component on the least
  mature layer in the stack.

Practical sequencing regardless of option: bring up probe-rs/defmt over the
Minima's SWD connector first (it is also the brick-recovery path), then the
flash-LP driver with on-target tests, then the CAN port — and only then decide
whether embassy-boot's rollback is worth the slot-halving (Option B can be
layered on later since the NorFlash driver is shared).
