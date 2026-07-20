//! Code-flash program/erase + data-flash read gate for the RA4M1 flash-LP
//! (FACI-LP) sequencer — blocking driver, `embedded-storage` `NorFlash`-shaped
//! (U1.1, docs/plan.md §6).
//!
//! # Why raw MMIO (deviation from rules/embedded.md, sanctioned)
//!
//! The `R_FACI_LP` register block is missing from both RA4M1 PACs (U0.2
//! finding), so this module is the plan §9 Q2 fallback: a **contained**
//! raw-MMIO driver. Every register access goes through a named accessor in
//! [`faci`]/[`fcache`] with the register offset taken from the Renesas CMSIS
//! header for this exact part (`vendor/ra4m1-headers/R7FA4M1AB.h`,
//! `R_FACI_LP_Type` @ `0x407E_C000`) and the sequence semantics from the
//! RA4M1 HW manual R01UM0007 flash-memory chapter, cross-checked against the
//! vendored, bench-proven FSP driver
//! (`vendor/tinyusb/.../src/r_flash_lp/r_flash_lp.c`, `r_flash_lp_*`
//! functions cited per leaf below). No other module may touch FACI registers.
//!
//! # Blocking-exception contract (docs/plan.md §4)
//!
//! The CPU cannot fetch from code flash while the sequencer is in code-flash
//! P/E mode, so each block erase / program session is one synchronous,
//! IRQ-masked, **RAM-resident** critical section. The `ram_*` leaf functions
//! carry `#[link_section = ".data"]` + `#[inline(never)]` (cortex-m-rt copies
//! `.data` to SRAM at boot) and avoid flash fetches by construction:
//!
//! - no `defmt`, no panic paths (raw pointers only — no slice indexing, no
//!   `unwrap`), no trait dispatch, no `core` calls that could stay in flash;
//! - no runtime division/modulo (no `__aeabi_*` libcalls) — all timing and
//!   timeout constants are computed at `const` time;
//! - register accessors are `#[inline(always)]`, so they dissolve into the
//!   RAM leaves; leaf-to-leaf calls are RAM-to-RAM;
//! - delays are a local `asm!` SUBS/BNE loop, not `cortex_m::asm::delay`.
//!
//! A `#[used]` anchor table ([`RAM_FN_ANCHOR`]) is retained defensively to
//! keep every leaf alive even under aggressive inlining/`--gc-sections`, so
//! RAM residency stays provable with `llvm-nm` (leaf symbols must resolve to
//! `0x2000_xxxx`) independent of how the tasks happen to call the driver.
//!
//! # Preconditions (owned by `ra4m1_embassy::clocks::init_hoco_48mhz`)
//!
//! - FCLK = 24 MHz ([`FCLK_HZ`]) — notified to the sequencer via `FISR.PCKA`.
//! - High-speed operating mode (`OPCCR.OPCM = 0`; mandatory for ICLK 48 MHz),
//!   so the FPMCR sequences below never need the LVPE variants
//!   (FSP `r_flash_lp_cf_enter_pe_mode`, the `OPCM == 0` branch).
//!
//! # What data-flash **P/E** would need (not in v1)
//!
//! The FACI plumbing here already supports it: `ram_df_pe_enter` +
//! [`faci::write_fasr`]`(0)` + per-byte programs via `FWBL0` with the P/E
//! address offset `0xFE00_0000 - 0x4010_0000` (FSP
//! `FLASH_LP_DATAFLASH_ADDR_OFFSET`, `r_flash_lp_df_write_operation`). Only
//! the safe wrappers and the DF timeout constants are missing.

use core::sync::atomic::{AtomicBool, Ordering};

use boot_logic::memory_map::{
    APP_BASE, CODE_FLASH_SIZE, FLASH_BLOCK_SIZE, FLASH_WRITE_UNIT, SRAM_BASE, SRAM_END,
};
use embedded_storage::nor_flash::{
    ErrorType, NorFlash, NorFlashError, NorFlashErrorKind, ReadNorFlash,
};
use ra4m1_embassy::clocks::{FCLK_HZ, ICLK_HZ};

// ---------------------------------------------------------------------------
// Memory map of the peripheral windows this driver owns
// ---------------------------------------------------------------------------

/// Data flash read window base (R01UM0007 address space; SPEC §1.1).
pub const DATA_FLASH_BASE: u32 = 0x4010_0000;
/// Data flash size: 8 KB on the R7FA4M1AB (SPEC §1.1).
pub const DATA_FLASH_SIZE: u32 = 0x2000;

// ---------------------------------------------------------------------------
// FACI command/mode constants (R01UM0007 flash chapter; values cross-checked
// against FSP r_flash_lp.c FLASH_LP_* defines)
// ---------------------------------------------------------------------------

/// `DFLCTL.DFLEN` — data-flash read enable (tDSTOP settle after 0→1).
const DFLCTL_DFLEN: u8 = 0x01;

/// FENTRYR write keys: high byte `0xAA`, low bits select the P/E mode.
const FENTRYR_CF_PE_ENTRY: u16 = 0xAA01;
const FENTRYR_DF_PE_ENTRY: u16 = 0xAA80;
const FENTRYR_READ: u16 = 0xAA00;
/// FENTRYR read-back value while code-flash P/E mode is active.
const FENTRYR_CF_PE_ACTIVE: u16 = 0x0001;

/// FPMCR mode values (FMS2/FMS1/FMS0 + RPDIS encodings; FSP `FLASH_LP_*`).
const FPMCR_READ: u8 = 0x08;
const FPMCR_DISCHARGE_1: u8 = 0x12;
const FPMCR_DISCHARGE_2: u8 = 0x92;
const FPMCR_CF_PE: u8 = 0x82;
const FPMCR_DF_PE: u8 = 0x10;

/// FPR unlock key for the FPMCR write sequence (value / !value / value).
const FPR_UNLOCK: u32 = 0xA5;

/// FCR software commands (`CMD` field + `OPST` bit 7).
const FCR_PROGRAM: u8 = 0x81;
const FCR_ERASE: u8 = 0x84;
const FCR_CLEAR: u8 = 0x00;

/// `FSTATR1.FRDY` — sequencer end-status flag (bit 6).
const FSTATR1_FRDY: u32 = 1 << 6;

/// FSTATR2 error masks per operation (FSP `FLASH_LP_FSTATR2_*_ERROR_BITS`):
/// erase = ILGLERR|ERERR, program = ILGLERR|PRGERR.
const FSTATR2_ERASE_ERRORS: u32 = 0x11;
const FSTATR2_PROGRAM_ERRORS: u32 = 0x12;

/// `FISR.PCKA` flash clock notification: FCLK in MHz minus 1, 5-bit field on
/// flash-LP v3 (FSP `r_flash_lp_set_fisr`). 24 MHz → 23.
const FISR_PCKA: u32 = FCLK_HZ / 1_000_000 - 1;
const FISR_PCKA_MASK: u32 = 0x3F;

// ---------------------------------------------------------------------------
// Timing (const-derived — no runtime division anywhere near the leaves)
// ---------------------------------------------------------------------------

const ICLK_MHZ: u32 = ICLK_HZ / 1_000_000;

/// SUBS/BNE delay loop ≈ 4 CPU cycles per iteration (FSP
/// `FLASH_LP_DELAY_LOOP_CYCLES`; conservative for zero-wait SRAM execution).
const DELAY_LOOP_CYCLES: u32 = 4;
const fn us_to_delay_loops(us: u32) -> u32 {
    us * ICLK_MHZ / DELAY_LOOP_CYCLES
}
/// tDIS ≥ 2.5 µs mode-discharge wait (FSP `FLASH_LP_WAIT_TDIS` = 3 µs).
const TDIS_DELAY_LOOPS: u32 = us_to_delay_loops(3);
/// tMS ≥ 5 µs mode-setup wait, high-speed mode (FSP `FLASH_LP_WAIT_TMS_HIGH`).
const TMS_HIGH_DELAY_LOOPS: u32 = us_to_delay_loops(6);
/// tDSTOP ≥ 6 µs data-flash access settle (FSP `FLASH_LP_WAIT_TDSTOP`;
/// SPEC §2.4 `DATA_FLASH_TDSTOP_SETTLE_US`).
const TDSTOP_DELAY_LOOPS: u32 = us_to_delay_loops(6);

/// tDSTOP settle for the *flash-resident* gate path, in CPU cycles for
/// `cortex_m::asm::delay` (≥ 6 µs at ICLK 48 MHz, 2× margin).
const TDSTOP_SETTLE_CYCLES: u32 = 6 * ICLK_MHZ * 2;

/// Status-poll loop ≥ 6 CPU cycles per iteration (FSP
/// `FLASH_LP_CYCLES_MINIMUM_PER_TIMEOUT_LOOP`).
const CYCLES_PER_POLL_LOOP: u32 = 6;
/// Datasheet-max 2 KB code-flash block erase 355 ms at FCLK 1 MHz worst case
/// (FSP `FLASH_LP_MAX_ERASE_CF_BLOCK_TIME_US`), in poll-loop counts at ICLK.
const ERASE_BLOCK_TIMEOUT_LOOPS: u32 = 355_000 * ICLK_MHZ / CYCLES_PER_POLL_LOOP;
/// Datasheet-max 8-byte code-flash program 1411 µs (FSP
/// `FLASH_LP_MAX_WRITE_CF_TIME_US`), in poll-loop counts at ICLK.
const WRITE_UNIT_TIMEOUT_LOOPS: u32 = 1_411 * ICLK_MHZ / CYCLES_PER_POLL_LOOP;
/// FENTRYR / cache-invalidate confirmation polls (FSP
/// `FLASH_LP_FRDY_CMD_TIMEOUT`).
const FENTRYR_TIMEOUT_LOOPS: u32 = 19_200;

/// Cap on one IRQ-masked program session: one erase block's worth of 8-byte
/// units, so a full-block program is a single critical section (plan §4) and
/// longer writes are chunked at block-size boundaries.
const PROGRAM_SESSION_MAX_BYTES: usize = FLASH_BLOCK_SIZE as usize;

// ---------------------------------------------------------------------------
// Leaf status word (returned by the RAM leaves; folded into FlashError)
// ---------------------------------------------------------------------------

/// Leaf success. Non-zero: bits 0..6 = `FSTATR2` error snapshot masked by the
/// operation's error bits; bit 8 = FRDY poll timeout; bit 9 = P/E-exit
/// (FENTRYR clear / cache-invalidate) timeout.
const STATUS_OK: u32 = 0;
const STATUS_FRDY_TIMEOUT: u32 = 1 << 8;
const STATUS_EXIT_TIMEOUT: u32 = 1 << 9;

// ---------------------------------------------------------------------------
// Error type
// ---------------------------------------------------------------------------

/// Flash driver failure. `raw` fields carry the leaf status word (see
/// `STATUS_*`): low bits are the `FSTATR2` error snapshot, bit 8 an FRDY
/// timeout, bit 9 a P/E-exit timeout. After any `Erase`/`Program`/`Init`
/// error the leaf has already software-reset the FACI (`FRESETR`) and
/// returned it to read mode, so the driver remains usable.
#[derive(Debug, Clone, Copy, PartialEq, Eq, defmt::Format)]
#[non_exhaustive]
pub enum FlashError {
    /// R3 guard (tightened, plan §8): erase/program below `APP_BASE`
    /// (`0x8000`) refused — the bootloader never writes its own region.
    Protected,
    /// Address/length not aligned to the required unit (8 B program,
    /// 2 KB erase).
    NotAligned,
    /// Range leaves the device (code flash 256 KB / data flash 8 KB window).
    OutOfBounds,
    /// Program source buffer is not in SRAM — during code-flash P/E the
    /// source must not itself live in (unreadable) code flash.
    SourceNotInRam,
    /// Block erase failed (FACI reset + read mode restored).
    Erase { raw: u32 },
    /// 8-byte-unit program failed (FACI reset + read mode restored).
    Program { raw: u32 },
    /// FISR flash-clock-notification round trip failed — no P/E is safe
    /// until it succeeds (retried on the next operation).
    Init { raw: u32 },
    /// Read-back verify mismatch at the given code-flash address.
    VerifyMismatch { addr: u32 },
    /// SPEC §2.4/R34 gate: data-flash readability not proven — the read was
    /// refused (caller falls back to compiled defaults).
    DataFlashNotReadable,
}

impl NorFlashError for FlashError {
    fn kind(&self) -> NorFlashErrorKind {
        match self {
            FlashError::NotAligned => NorFlashErrorKind::NotAligned,
            FlashError::OutOfBounds => NorFlashErrorKind::OutOfBounds,
            _ => NorFlashErrorKind::Other,
        }
    }
}

// ---------------------------------------------------------------------------
// Named register accessors — the ONLY place FACI/FCACHE addresses appear.
// All accessors are #[inline(always)] so they compile into their (possibly
// RAM-resident) callers instead of becoming flash-resident calls.
// ---------------------------------------------------------------------------

/// `R_FACI_LP` @ `0x407E_C000` (R7FA4M1AB.h `R_FACI_LP_BASE`). Offsets below
/// are the `R_FACI_LP_Type` struct offsets from that header; semantics from
/// R01UM0007's flash-memory chapter register descriptions.
mod faci {
    const BASE: u32 = 0x407E_C000;

    const DFLCTL: u32 = BASE + 0x090; // u8  Data Flash Control (DFLEN)
    const FPMCR: u32 = BASE + 0x100; // u8  Flash P/E Mode Control
    const FASR: u32 = BASE + 0x104; // u8  Flash Area Select (EXS)
    const FSARL: u32 = BASE + 0x108; // u16 Processing Start Address L
    const FSARH: u32 = BASE + 0x110; // u16 Processing Start Address H
    const FCR: u32 = BASE + 0x114; // u8  Flash Control (CMD/OPST)
    const FEARL: u32 = BASE + 0x118; // u16 Processing End Address L
    const FEARH: u32 = BASE + 0x120; // u32 Processing End Address H
    const FRESETR: u32 = BASE + 0x124; // u32 Flash Reset (FRESET)
    const FSTATR1: u32 = BASE + 0x12C; // u32 Status 1 (FRDY bit 6), RO
    const FWBL0: u32 = BASE + 0x130; // u32 Write Buffer L0 (data 15:0)
    const FWBH0: u32 = BASE + 0x138; // u32 Write Buffer H0 (data 31:16)
    const FWBL1: u32 = BASE + 0x140; // u32 Write Buffer L1 (data 47:32)
    const FWBH1: u32 = BASE + 0x144; // u32 Write Buffer H1 (data 63:48)
    const FPR: u32 = BASE + 0x180; // u32 Protection Unlock, WO
    const FISR: u32 = BASE + 0x1D8; // u32 Initial Setting (PCKA/SAS)
    const FSTATR2: u32 = BASE + 0x1F0; // u32 Status 2 (error flags), RO
    const FENTRYR: u32 = BASE + 0x3FB2; // u16 P/E Mode Entry

    /// Read `DFLCTL` (safe: status read of an always-clocked FACI register).
    #[inline(always)]
    pub fn read_dflctl() -> u8 {
        // SAFETY: DFLCTL is a valid, always-clocked MMIO byte register at a
        // constant address inside R_FACI_LP; reading has no side effects.
        unsafe { (DFLCTL as *const u8).read_volatile() }
    }

    /// Write `DFLCTL`. Setting DFLEN 0→1 requires a tDSTOP (≥ 6 µs) settle
    /// before any data-flash read (R01UM0007; SPEC §2.4).
    ///
    /// # Safety
    /// Caller owns the settle-time contract above.
    #[inline(always)]
    pub unsafe fn write_dflctl(v: u8) {
        // SAFETY: valid MMIO byte register; any u8 write is accepted by
        // hardware — the settle contract is the caller's per this fn's docs.
        unsafe { (DFLCTL as *mut u8).write_volatile(v) }
    }

    /// FPMCR mode write via the FPR-unlocked value/!value/value sequence
    /// (R01UM0007 "FPMCR write procedure"; FSP `r_flash_lp_write_fpmcr`),
    /// then a read-back to flush the bus write buffer.
    ///
    /// # Safety
    /// `v` must be one of the documented FPMCR mode encodings and the write
    /// must occur at the documented point of a P/E entry/exit sequence.
    #[inline(always)]
    pub unsafe fn write_fpmcr_sequence(v: u8) {
        // SAFETY: the FPR unlock + triple-write is exactly the documented
        // sequence; all four accesses are valid MMIO within R_FACI_LP.
        unsafe {
            (FPR as *mut u32).write_volatile(super::FPR_UNLOCK);
            (FPMCR as *mut u8).write_volatile(v);
            (FPMCR as *mut u8).write_volatile(!v);
            (FPMCR as *mut u8).write_volatile(v);
            let _ = (FPMCR as *const u8).read_volatile();
        }
    }

    /// Select the user area (`FASR.EXS = 0`) — this driver never touches the
    /// extra area (startup/access-window/OCD ID programming).
    ///
    /// # Safety
    /// Must only be issued while the sequencer is idle in a P/E mode.
    #[inline(always)]
    pub unsafe fn select_user_area() {
        // SAFETY: valid MMIO byte register; 0 = user area per R01UM0007.
        unsafe { (FASR as *mut u8).write_volatile(0) }
    }

    /// Load the processing start address registers (`FSARH:FSARL`).
    ///
    /// # Safety
    /// P/E-mode sequencer setup only; `addr` must be a P/E-space address.
    #[inline(always)]
    pub unsafe fn write_start_addr(addr: u32) {
        // SAFETY: valid MMIO u16 registers; masks make the truncation to the
        // documented FSAR fields explicit and lossless.
        unsafe {
            ((FSARH as *mut u16).write_volatile(((addr >> 16) & 0xFFFF) as u16));
            ((FSARL as *mut u16).write_volatile((addr & 0xFFFF) as u16));
        }
    }

    /// Load the processing end address registers (`FEARH:FEARL`) — the last
    /// byte address (inclusive) of an erase/blank-check range.
    ///
    /// # Safety
    /// P/E-mode sequencer setup only.
    #[inline(always)]
    pub unsafe fn write_end_addr(addr: u32) {
        // SAFETY: valid MMIO registers (FEARH is a u32 register whose low 16
        // bits are the FEAR high half); masked, lossless field writes.
        unsafe {
            ((FEARH as *mut u32).write_volatile((addr >> 16) & 0xFFFF));
            ((FEARL as *mut u16).write_volatile((addr & 0xFFFF) as u16));
        }
    }

    /// Load the four 16-bit program-data write buffers (64-bit unit,
    /// little-endian: L0 = bytes 0..2 … H1 = bytes 6..8).
    ///
    /// # Safety
    /// P/E-mode sequencer setup only, before the program `FCR` command.
    #[inline(always)]
    pub unsafe fn write_program_data(halves: [u16; 4]) {
        // SAFETY: valid MMIO u32 registers; low 16 bits carry program data
        // per R01UM0007 (FSP r_flash_lp_cf_write_operation order).
        unsafe {
            ((FWBL0 as *mut u32).write_volatile(u32::from(halves[0])));
            ((FWBH0 as *mut u32).write_volatile(u32::from(halves[1])));
            ((FWBL1 as *mut u32).write_volatile(u32::from(halves[2])));
            ((FWBH1 as *mut u32).write_volatile(u32::from(halves[3])));
        }
    }

    /// Issue an `FCR` software command (or `FCR_CLEAR` to end one).
    ///
    /// # Safety
    /// `v` must be a documented command; erase/program commands start
    /// destructive sequencer operations — caller owns mode + range setup.
    #[inline(always)]
    pub unsafe fn write_fcr(v: u8) {
        // SAFETY: valid MMIO byte register; command validity per docs above.
        unsafe { (FCR as *mut u8).write_volatile(v) }
    }

    /// Read `FSTATR1` (safe: side-effect-free status read).
    #[inline(always)]
    pub fn read_fstatr1() -> u32 {
        // SAFETY: valid, read-only MMIO status register.
        unsafe { (FSTATR1 as *const u32).read_volatile() }
    }

    /// Read `FSTATR2` (safe: side-effect-free status read).
    #[inline(always)]
    pub fn read_fstatr2() -> u32 {
        // SAFETY: valid, read-only MMIO status register.
        unsafe { (FSTATR2 as *const u32).read_volatile() }
    }

    /// Read `FENTRYR` (safe: side-effect-free mode read).
    #[inline(always)]
    pub fn read_fentryr() -> u16 {
        // SAFETY: valid MMIO u16 register; reading does not change mode.
        unsafe { (FENTRYR as *const u16).read_volatile() }
    }

    /// Write `FENTRYR` (P/E mode entry/exit; key `0xAA` in the high byte).
    ///
    /// # Safety
    /// Entering code-flash P/E makes code flash unfetchable: the caller must
    /// be executing from RAM with IRQs masked until read mode is confirmed.
    #[inline(always)]
    pub unsafe fn write_fentryr(v: u16) {
        // SAFETY: valid MMIO u16 register; mode-change contract per docs.
        unsafe { (FENTRYR as *mut u16).write_volatile(v) }
    }

    /// Toggle the FACI software reset (`FRESETR.FRESET` 1 → 0), aborting any
    /// stuck sequencer operation (FSP `r_flash_lp_reset`).
    ///
    /// # Safety
    /// Must be issued from a P/E mode; caller transitions back to read mode.
    #[inline(always)]
    pub unsafe fn pulse_freset() {
        // SAFETY: valid MMIO u32 register; the 1-then-0 pulse is the
        // documented software-reset sequence.
        unsafe {
            ((FRESETR as *mut u32).write_volatile(1));
            ((FRESETR as *mut u32).write_volatile(0));
        }
    }

    /// Read-modify-write `FISR.PCKA` (flash clock notification), preserving
    /// the SAS bits. Only writable from data-flash P/E mode (FSP
    /// `r_flash_lp_set_fisr`).
    ///
    /// # Safety
    /// Caller must have the sequencer in data-flash P/E mode.
    #[inline(always)]
    pub unsafe fn write_fisr_pcka(pcka: u32) {
        // SAFETY: valid MMIO u32 register; RMW preserves the non-PCKA bits.
        unsafe {
            let old = (FISR as *const u32).read_volatile();
            ((FISR as *mut u32).write_volatile((old & !super::FISR_PCKA_MASK) | pcka));
        }
    }
}

/// `R_FCACHE` @ `0x4001_C000` (R7FA4M1AB.h `R_FCACHE_BASE`). The flash cache
/// must be off while the sequencer is in code-flash P/E mode and invalidated
/// before re-enabling (FSP `R_BSP_FlashCacheDisable`/`Enable`).
mod fcache {
    const BASE: u32 = 0x4001_C000;
    const FCACHEE: u32 = BASE + 0x100; // u16 Flash Cache Enable
    const FCACHEIV: u32 = BASE + 0x104; // u16 Flash Cache Invalidate

    /// Read `FCACHEE` (safe: side-effect-free enable-bit read).
    #[inline(always)]
    pub fn read_enable() -> u16 {
        // SAFETY: valid MMIO u16 register; read-only access.
        unsafe { (FCACHEE as *const u16).read_volatile() }
    }

    /// Write `FCACHEE` (0 disable / 1 enable).
    ///
    /// # Safety
    /// Enable only after a completed invalidate; disable before code-flash
    /// P/E entry.
    #[inline(always)]
    pub unsafe fn write_enable(v: u16) {
        // SAFETY: valid MMIO u16 register; 0/1 are the documented values.
        unsafe { (FCACHEE as *mut u16).write_volatile(v) }
    }

    /// Start a cache invalidate (`FCACHEIV = 1`; hardware clears it when
    /// done).
    ///
    /// # Safety
    /// Only while the cache is disabled, before re-enabling it.
    #[inline(always)]
    pub unsafe fn start_invalidate() {
        // SAFETY: valid MMIO u16 register; 1 is the documented trigger.
        unsafe { (FCACHEIV as *mut u16).write_volatile(1) }
    }

    /// Read `FCACHEIV` (safe: completion poll).
    #[inline(always)]
    pub fn read_invalidate() -> u16 {
        // SAFETY: valid MMIO u16 register; read-only access.
        unsafe { (FCACHEIV as *const u16).read_volatile() }
    }
}

/// DSB + ISB: order the preceding MMIO mode change against subsequent
/// execution (cache disable / FENTRYR writes). Inline asm so nothing here
/// depends on `cortex-m`'s inlining behavior inside RAM leaves.
#[inline(always)]
fn dsb_isb() {
    // SAFETY: barrier instructions only — no memory or register operands.
    unsafe { core::arch::asm!("dsb", "isb", options(nostack, preserves_flags)) }
}

// ---------------------------------------------------------------------------
// RAM-resident leaves. Every function below runs from SRAM (`.data`) and is
// free of flash fetches by construction (module-header rules). They are
// pub(crate) + anchored so their RAM addresses are verifiable with llvm-nm
// before U1.3/U1.4 reference the driver.
// ---------------------------------------------------------------------------

/// Busy-wait `loops` SUBS/BNE iterations (≈ 4 cycles each at 48 MHz from
/// zero-wait SRAM). FSP `r_flash_lp_delay_us` equivalent.
///
/// # Safety
/// None beyond the module's RAM-residency contract (kept `unsafe fn` for
/// uniformity with the other leaves it is anchored alongside).
#[link_section = ".data"]
#[inline(never)]
pub(crate) unsafe fn ram_delay_loops(loops: u32) {
    if loops == 0 {
        return;
    }
    let mut n = loops;
    // SAFETY: self-contained counted loop on one register; no memory access.
    unsafe {
        core::arch::asm!(
            "1:",
            "subs {0}, {0}, #1",
            "bne 1b",
            inout(reg) n,
            options(nomem, nostack),
        );
    }
    let _ = n;
}

/// Enter code-flash P/E mode. Returns the FCACHEE snapshot to restore at
/// exit. Sequence: cache off → `FENTRYR = 0xAA01` → FPMCR Discharge-1 →
/// tDIS → Discharge-2 → Code-flash-P/E → tMS (R01UM0007 "read mode to code
/// flash P/E mode" procedure; FSP `r_flash_lp_cf_enter_pe_mode`, high-speed
/// branch — see the module-header precondition).
///
/// # Safety
/// Caller must be executing this and everything until [`ram_pe_exit`]
/// returns from RAM, with IRQs masked: code flash is unfetchable in between.
#[link_section = ".data"]
#[inline(never)]
pub(crate) unsafe fn ram_cf_pe_enter() -> u32 {
    let fcache_was_enabled = u32::from(fcache::read_enable() & 1);
    // SAFETY: cache disable before P/E entry, per this leaf's sequence doc.
    unsafe { fcache::write_enable(0) };
    dsb_isb();
    // SAFETY: RAM residency + IRQ masking are this fn's own safety contract.
    unsafe {
        faci::write_fentryr(FENTRYR_CF_PE_ENTRY);
        dsb_isb();
        faci::write_fpmcr_sequence(FPMCR_DISCHARGE_1);
        ram_delay_loops(TDIS_DELAY_LOOPS);
        faci::write_fpmcr_sequence(FPMCR_DISCHARGE_2);
        faci::write_fpmcr_sequence(FPMCR_CF_PE);
        ram_delay_loops(TMS_HIGH_DELAY_LOOPS);
    }
    fcache_was_enabled
}

/// Enter data-flash P/E mode: `FENTRYR = 0xAA80` → tDSTOP → FPMCR
/// Data-flash-P/E (R01UM0007 "read mode to data flash P/E mode" procedure;
/// FSP `r_flash_lp_df_enter_pe_mode`, v3 high-speed branch). Code flash
/// stays fetchable in this mode; RAM residency is kept anyway so the shared
/// exit/reset paths are uniform (as FSP does).
///
/// # Safety
/// Sequencer must be in read mode (`FENTRYR == 0`); pair with
/// [`ram_pe_exit`].
#[link_section = ".data"]
#[inline(never)]
pub(crate) unsafe fn ram_df_pe_enter() {
    // SAFETY: documented entry sequence; mode precondition is this fn's
    // contract.
    unsafe {
        faci::write_fentryr(FENTRYR_DF_PE_ENTRY);
        dsb_isb();
        ram_delay_loops(TDSTOP_DELAY_LOOPS);
        faci::write_fpmcr_sequence(FPMCR_DF_PE);
    }
}

/// Return the sequencer to read mode from either P/E mode. From code-flash
/// P/E: Discharge-2 → tDIS → Discharge-1 first. Then Read mode → tMS →
/// `FENTRYR = 0xAA00` → poll cleared → restore the flash cache
/// (invalidate-then-enable) if `restore_fcache != 0` (R01UM0007 "P/E mode to
/// read mode" procedure; FSP `r_flash_lp_pe_mode_exit`).
///
/// Returns `STATUS_OK` or `STATUS_EXIT_TIMEOUT`.
///
/// # Safety
/// Same RAM-residency/IRQ contract as [`ram_cf_pe_enter`]; `restore_fcache`
/// must be the value that leaf returned (or 0 for data-flash sessions).
#[link_section = ".data"]
#[inline(never)]
pub(crate) unsafe fn ram_pe_exit(restore_fcache: u32) -> u32 {
    let was_cf_pe = faci::read_fentryr() == FENTRYR_CF_PE_ACTIVE;
    // SAFETY: documented exit sequence; contract inherited from this fn.
    unsafe {
        if was_cf_pe {
            faci::write_fpmcr_sequence(FPMCR_DISCHARGE_2);
            ram_delay_loops(TDIS_DELAY_LOOPS);
            faci::write_fpmcr_sequence(FPMCR_DISCHARGE_1);
        }
        faci::write_fpmcr_sequence(FPMCR_READ);
        ram_delay_loops(TMS_HIGH_DELAY_LOOPS);
        faci::write_fentryr(FENTRYR_READ);
        dsb_isb();
    }
    let mut status = STATUS_OK;
    let mut n = FENTRYR_TIMEOUT_LOOPS;
    while faci::read_fentryr() != 0 {
        if n == 0 {
            status = STATUS_EXIT_TIMEOUT;
            break;
        }
        n -= 1;
    }
    if was_cf_pe && restore_fcache != 0 {
        // SAFETY: cache is currently disabled (since ram_cf_pe_enter);
        // invalidate completes before re-enable, per the accessor docs.
        unsafe { fcache::start_invalidate() };
        let mut n = FENTRYR_TIMEOUT_LOOPS;
        while fcache::read_invalidate() != 0 {
            if n == 0 {
                status |= STATUS_EXIT_TIMEOUT;
                break;
            }
            n -= 1;
        }
        // SAFETY: invalidate finished (or timed out — enabling anyway
        // matches FSP's unbounded wait semantics as closely as we can
        // while staying bounded); restoring the snapshot state.
        unsafe { fcache::write_enable(1) };
        dsb_isb();
    }
    status
}

/// Wait for the issued FCR command to finish: poll `FRDY == 1`, write
/// `FCR = 0x00`, poll `FRDY == 0` (R01UM0007 erase/program flowcharts; FSP
/// `r_flash_lp_command_finish`). Returns `STATUS_OK` or
/// `STATUS_FRDY_TIMEOUT`.
///
/// # Safety
/// Only between a P/E entry and exit, after an FCR command was issued; RAM
/// residency + IRQ masking per [`ram_cf_pe_enter`].
#[link_section = ".data"]
#[inline(never)]
pub(crate) unsafe fn ram_command_finish(timeout_loops: u32) -> u32 {
    let mut n = timeout_loops;
    while faci::read_fstatr1() & FSTATR1_FRDY == 0 {
        if n == 0 {
            return STATUS_FRDY_TIMEOUT;
        }
        n -= 1;
    }
    // SAFETY: FCR_CLEAR ends the command per the documented finish sequence.
    unsafe { faci::write_fcr(FCR_CLEAR) };
    let mut n = timeout_loops;
    while faci::read_fstatr1() & FSTATR1_FRDY != 0 {
        if n == 0 {
            return STATUS_FRDY_TIMEOUT;
        }
        n -= 1;
    }
    STATUS_OK
}

/// Error recovery while in a P/E mode: FACI software reset (`FRESETR`
/// pulse), then transition to read mode (FSP `r_flash_lp_reset`, the
/// already-in-P/E branch used by `r_flash_lp_wait_for_ready` error paths).
///
/// # Safety
/// Sequencer must currently be in a P/E mode; same RAM/IRQ contract.
#[link_section = ".data"]
#[inline(never)]
pub(crate) unsafe fn ram_reset_and_exit(restore_fcache: u32) -> u32 {
    // SAFETY: in P/E mode per this fn's contract — the documented context
    // for the FRESET pulse; exit follows immediately.
    unsafe {
        faci::pulse_freset();
        ram_pe_exit(restore_fcache)
    }
}

/// Erase one 2 KB code-flash block: CF P/E entry → user area → FSAR/FEAR =
/// block start/end → `FCR = 0x84` → finish → `FSTATR2` erase-error check →
/// exit (R01UM0007 code-flash erase flowchart; FSP `r_flash_lp_cf_erase` +
/// `r_flash_lp_process_command`). ~ms typical, 355 ms datasheet max.
///
/// # Safety
/// - `block_addr` must be 2 KB aligned and inside the app+metadata region —
///   erasing the bootloader's own blocks pulls the code out from under the
///   PC on the next flash fetch ([`FlashLp`] enforces this).
/// - IRQs masked for the whole call; call must come from flash-resident code
///   (this leaf runs from RAM; the return address stays valid).
#[link_section = ".data"]
#[inline(never)]
pub(crate) unsafe fn ram_cf_erase_block(block_addr: u32) -> u32 {
    // SAFETY: contract inherited from this fn (range + IRQs masked).
    let fcache = unsafe { ram_cf_pe_enter() };
    // SAFETY: in CF P/E mode; user-area erase of the given block only.
    unsafe {
        faci::select_user_area();
        faci::write_start_addr(block_addr);
        faci::write_end_addr(block_addr + (FLASH_BLOCK_SIZE - 1));
        faci::write_fcr(FCR_ERASE);
    }
    // SAFETY: command issued above; still in P/E mode.
    let mut status = unsafe { ram_command_finish(ERASE_BLOCK_TIMEOUT_LOOPS) };
    status |= faci::read_fstatr2() & FSTATR2_ERASE_ERRORS;
    if status != STATUS_OK {
        // SAFETY: in P/E mode after a failed command — the documented
        // FRESET recovery context.
        return status | unsafe { ram_reset_and_exit(fcache) };
    }
    // SAFETY: matching exit for the entry above.
    unsafe { ram_pe_exit(fcache) }
}

/// Program `units` × 8 bytes from `src` (SRAM) to `dest` (code flash), all
/// inside one CF P/E session: per unit FSAR → FWB×4 → `FCR = 0x81` → finish
/// → `FSTATR2` program-error check (R01UM0007 code-flash program flowchart;
/// FSP `r_flash_lp_cf_write` + `r_flash_lp_cf_write_operation`). Worst case
/// 1411 µs/unit (datasheet max), so a full 2 KB session is bounded ≈ 361 ms;
/// typical is low single-digit ms.
///
/// # Safety
/// - `dest` 8-byte aligned, inside the app+metadata region; `dest + units*8`
///   must not overflow past the region ([`FlashLp`] enforces this).
/// - `src..src + units*8` must be readable **SRAM** (code flash is
///   unreadable during the session) and must not alias FACI registers.
/// - IRQs masked for the whole call.
#[link_section = ".data"]
#[inline(never)]
pub(crate) unsafe fn ram_cf_program(dest: u32, src: *const u8, units: u32) -> u32 {
    // SAFETY: contract inherited from this fn.
    let fcache = unsafe { ram_cf_pe_enter() };
    // SAFETY: in CF P/E mode.
    unsafe { faci::select_user_area() };
    let mut addr = dest;
    let mut p = src;
    let mut remaining = units;
    while remaining != 0 {
        // SAFETY: p..p+8 is readable SRAM per this fn's contract; explicit
        // byte loads avoid any memcpy libcall (flash-resident) and slice
        // panics. Little-endian packing per faci::write_program_data docs.
        let halves: [u16; 4] = unsafe {
            [
                u16::from(p.read()) | (u16::from(p.add(1).read()) << 8),
                u16::from(p.add(2).read()) | (u16::from(p.add(3).read()) << 8),
                u16::from(p.add(4).read()) | (u16::from(p.add(5).read()) << 8),
                u16::from(p.add(6).read()) | (u16::from(p.add(7).read()) << 8),
            ]
        };
        // SAFETY: in CF P/E mode; unit-program of 8 in-range bytes.
        unsafe {
            faci::write_start_addr(addr);
            faci::write_program_data(halves);
            faci::write_fcr(FCR_PROGRAM);
        }
        // SAFETY: command issued above.
        let mut status = unsafe { ram_command_finish(WRITE_UNIT_TIMEOUT_LOOPS) };
        status |= faci::read_fstatr2() & FSTATR2_PROGRAM_ERRORS;
        if status != STATUS_OK {
            // SAFETY: failed-command recovery, in P/E mode.
            return status | unsafe { ram_reset_and_exit(fcache) };
        }
        addr += FLASH_WRITE_UNIT;
        // SAFETY: stays within the caller-guaranteed src..src+units*8 range.
        p = unsafe { p.add(FLASH_WRITE_UNIT as usize) };
        remaining -= 1;
    }
    // SAFETY: matching exit for the entry above.
    unsafe { ram_pe_exit(fcache) }
}

/// Set the `FISR.PCKA` flash-clock notification (FCLK 24 MHz → 23) from
/// data-flash P/E mode — the only mode in which FISR is writable (FSP
/// `r_flash_lp_set_fisr`). Must succeed before any erase/program.
///
/// # Safety
/// Sequencer must be in read mode; IRQs masked across the call.
#[link_section = ".data"]
#[inline(never)]
pub(crate) unsafe fn ram_set_fisr() -> u32 {
    // SAFETY: DF P/E round trip per this fn's contract.
    unsafe {
        ram_df_pe_enter();
        faci::write_fisr_pcka(FISR_PCKA);
        ram_pe_exit(0)
    }
}

/// FACI software reset from read mode — the SPEC §2.4 gate's
/// `R_FLASH_LP_Reset` step: enter data-flash P/E mode if not already in a
/// P/E mode, pulse `FRESETR`, return to read mode (FSP `r_flash_lp_reset`).
/// Clears sequencer state a software reset (`NVIC_SystemReset`) may have
/// preserved (SPEC §9.1.2).
///
/// # Safety
/// IRQs masked across the call; RAM residency per the module contract.
#[link_section = ".data"]
#[inline(never)]
pub(crate) unsafe fn ram_df_reset() -> u32 {
    if faci::read_fentryr() == 0 {
        // SAFETY: read mode confirmed — the documented precondition.
        unsafe { ram_df_pe_enter() };
    }
    // SAFETY: now in a P/E mode — the documented FRESET context.
    unsafe {
        faci::pulse_freset();
        ram_pe_exit(0)
    }
}

/// Link-time anchor: keeps every RAM leaf alive through LTO (`#[used]`) and
/// the linker's `--gc-sections` (the `.flash_lp_anchor` section is `KEEP`ed
/// in `memory.x`). Retained defensively even though the tasks now call the
/// driver: it guarantees `llvm-nm` can prove the leaf symbols land in SRAM
/// regardless of the call graph. 40 bytes of flash; never read at runtime.
/// (`#[used(linker)]` would replace the memory.x KEEP, but it is unstable on
/// the pinned toolchain.)
struct RamFnAnchor(#[allow(dead_code)] [*const (); 10]); // linker-read only, never at runtime
                                                         // SAFETY: the array is never dereferenced or shared at runtime — it exists
                                                         // only so the linker sees a use of each leaf; fn addresses are plain values.
unsafe impl Sync for RamFnAnchor {}

#[link_section = ".flash_lp_anchor"]
#[used]
static RAM_FN_ANCHOR: RamFnAnchor = RamFnAnchor([
    ram_delay_loops as *const (),
    ram_cf_pe_enter as *const (),
    ram_df_pe_enter as *const (),
    ram_pe_exit as *const (),
    ram_command_finish as *const (),
    ram_reset_and_exit as *const (),
    ram_cf_erase_block as *const (),
    ram_cf_program as *const (),
    ram_set_fisr as *const (),
    ram_df_reset as *const (),
]);

// ---------------------------------------------------------------------------
// Safe driver
// ---------------------------------------------------------------------------

/// Guards the one [`FlashLp`] instance (the FACI has global state; two
/// handles could interleave P/E sequences).
static FLASH_TAKEN: AtomicBool = AtomicBool::new(false);

/// Blocking RA4M1 flash-LP driver owning all FACI access.
///
/// Each public erase/program call is one (or a chunked series of)
/// IRQ-masked, RAM-resident critical section(s) — the plan §4 blocking
/// exception. Bounds: erase ~ms typical / 355 ms max per 2 KB block; program
/// ~ms typical / ~361 ms max per 2 KB session (datasheet maxima via FSP
/// timeout constants). The `can_update` task chunks one block per loop
/// iteration, so other tasks stay live between blocks.
///
/// Data-flash **reads** are gated by [`FlashLp::data_flash_enable`]
/// (SPEC §2.4/R34); data-flash P/E is not implemented in v1 (module header).
pub struct FlashLp {
    /// FISR.PCKA clock notification done (one-time, latched on success).
    fisr_set: bool,
    /// SPEC §2.4 latched proof of data-flash readability.
    df_readable: bool,
}

impl FlashLp {
    /// Hand out the singleton driver. `None` after the first call.
    pub fn take() -> Option<Self> {
        if FLASH_TAKEN.swap(true, Ordering::AcqRel) {
            return None;
        }
        Some(Self {
            fisr_set: false,
            df_readable: false,
        })
    }

    /// R3 guard (tightened): erase/program must stay inside
    /// `[APP_BASE, CODE_FLASH_SIZE)` — app slot + metadata block; the
    /// metadata block is the last block, so its end is the flash end
    /// (const-asserted in `boot_logic::memory_map`).
    fn check_pe_range(addr: u32, len: u32) -> Result<(), FlashError> {
        debug_assert!(
            addr >= APP_BASE,
            "R3 violation: P/E below APP_BASE (bootloader region)"
        );
        if addr < APP_BASE {
            return Err(FlashError::Protected);
        }
        let end = addr.checked_add(len).ok_or(FlashError::OutOfBounds)?;
        debug_assert!(end <= CODE_FLASH_SIZE, "P/E past the end of code flash");
        if end > CODE_FLASH_SIZE {
            return Err(FlashError::OutOfBounds);
        }
        Ok(())
    }

    /// One-time FACI "open" (FSP `R_FLASH_LP_Open` equivalent): enable the
    /// data-flash window if it is off (with the tDSTOP settle — skipped when
    /// already on, exactly the FSP quirk SPEC §2.4 hardens against), then
    /// notify the sequencer of FCLK via FISR. Latched on success; retried on
    /// the next call after a failure.
    fn open_faci(&mut self) -> Result<(), FlashError> {
        if self.fisr_set {
            return Ok(());
        }
        if faci::read_dflctl() & DFLCTL_DFLEN == 0 {
            // SAFETY: settle contract honored by the delay just below.
            unsafe { faci::write_dflctl(DFLCTL_DFLEN) };
            cortex_m::asm::delay(TDSTOP_SETTLE_CYCLES);
        }
        // SAFETY: read mode + IRQs masked (interrupt::free) per the leaf's
        // contract; ~10 µs critical section.
        let raw = cortex_m::interrupt::free(|_| unsafe { ram_set_fisr() });
        if raw != STATUS_OK {
            return Err(FlashError::Init { raw });
        }
        self.fisr_set = true;
        Ok(())
    }

    /// The SPEC §2.4/R34 data-flash read gate. Proof of readability =
    /// Open-equivalent success **and** Reset-equivalent success, then an
    /// unconditional `DFLCTL.DFLEN = 1` re-assert plus a ≥ 6 µs tDSTOP
    /// settle. The result is latched; while `false`, every
    /// [`FlashLp::read_data_flash`] refuses (callers fall back to compiled
    /// defaults — fail-safe, never a locked core; SPEC §9.1.2).
    ///
    /// Call once per boot before any `DFU_CONFIG_ADDR` access, on **every**
    /// entry path.
    pub fn data_flash_enable(&mut self) -> bool {
        self.df_readable = false;
        // Step 1: Open — mirrors flash_lp_open_df_read_ready()'s
        // R_FLASH_LP_Open short-circuit (vendored C main.c).
        if self.open_faci().is_err() {
            return false;
        }
        // Step 2: Reset — flushes FACI state a software reset preserved.
        // SAFETY: IRQs masked; leaf handles both read-mode and dirty-P/E
        // entry states; µs-scale critical section.
        let raw = cortex_m::interrupt::free(|_| unsafe { ram_df_reset() });
        if raw != STATUS_OK {
            return false;
        }
        // Step 3: unconditional re-assert + settle, so the gate holds by
        // local proof rather than by driver internals (SPEC §2.4).
        // SAFETY: settle contract honored by the delay just below.
        unsafe { faci::write_dflctl(DFLCTL_DFLEN) };
        cortex_m::asm::delay(TDSTOP_SETTLE_CYCLES);
        self.df_readable = true;
        true
    }

    /// Latched result of the last [`FlashLp::data_flash_enable`].
    #[allow(dead_code)] // M1 consumes the gate via data_flash_enable's return; M2's DFU strings re-query
    pub fn data_flash_readable(&self) -> bool {
        self.df_readable
    }

    /// Volatile read from the data-flash window (`0x4010_0000..+8 KB`, e.g.
    /// the DFU config block at `boot_logic::dfu_config::DFU_CONFIG_ADDR`).
    /// `addr` is the absolute data-flash address. Refused wholesale until
    /// the gate has proven readability (R34).
    pub fn read_data_flash(&self, addr: u32, buf: &mut [u8]) -> Result<(), FlashError> {
        if !self.df_readable {
            return Err(FlashError::DataFlashNotReadable);
        }
        let len = u32::try_from(buf.len()).map_err(|_| FlashError::OutOfBounds)?;
        let end = addr.checked_add(len).ok_or(FlashError::OutOfBounds)?;
        if addr < DATA_FLASH_BASE || end > DATA_FLASH_BASE + DATA_FLASH_SIZE {
            return Err(FlashError::OutOfBounds);
        }
        for (i, byte) in buf.iter_mut().enumerate() {
            let src = addr as usize + i;
            // SAFETY: src is inside the memory-mapped data-flash window
            // (bounds-checked above) and DFLEN + tDSTOP were proven by the
            // gate; volatile raw-pointer read — never a reference into MMIO.
            *byte = unsafe { (src as *const u8).read_volatile() };
        }
        Ok(())
    }

    /// Erase one 2 KB block (one IRQ-masked RAM-resident critical section —
    /// the plan §4 unit the ERASE phase chunks per task-loop iteration).
    /// `block_addr` must be 2 KB aligned and inside the app+metadata region.
    pub fn erase_block(&mut self, block_addr: u32) -> Result<(), FlashError> {
        if !block_addr.is_multiple_of(FLASH_BLOCK_SIZE) {
            return Err(FlashError::NotAligned);
        }
        Self::check_pe_range(block_addr, FLASH_BLOCK_SIZE)?;
        self.open_faci()?;
        // SAFETY: range checked (R3), alignment checked, IRQs masked for the
        // whole block operation; the leaf runs from RAM.
        let raw = cortex_m::interrupt::free(|_| unsafe { ram_cf_erase_block(block_addr) });
        if raw != STATUS_OK {
            return Err(FlashError::Erase { raw });
        }
        Ok(())
    }

    /// Program `data` at code-flash address `addr`: 8-byte units, `addr` and
    /// `data.len()` multiples of 8, `data` in SRAM (it is read while code
    /// flash is unreadable). Sessions are chunked at 2 KB so one IRQ-masked
    /// window never exceeds a block (plan §4). Target must be erased.
    pub fn program(&mut self, addr: u32, data: &[u8]) -> Result<(), FlashError> {
        if data.is_empty() {
            return Ok(());
        }
        let len = u32::try_from(data.len()).map_err(|_| FlashError::OutOfBounds)?;
        if !addr.is_multiple_of(FLASH_WRITE_UNIT) || !len.is_multiple_of(FLASH_WRITE_UNIT) {
            return Err(FlashError::NotAligned);
        }
        Self::check_pe_range(addr, len)?;
        let src = data.as_ptr() as usize as u32;
        // Fail fast: a .rodata source would vanish mid-session (SPEC §9.1).
        if src < SRAM_BASE || src.saturating_add(len) > SRAM_END {
            return Err(FlashError::SourceNotInRam);
        }
        self.open_faci()?;
        let mut chunk_addr = addr;
        for chunk in data.chunks(PROGRAM_SESSION_MAX_BYTES) {
            // Lossless: chunk.len() <= 2048 and is a multiple of 8.
            let units = (chunk.len() / FLASH_WRITE_UNIT as usize) as u32;
            let ptr = chunk.as_ptr();
            // SAFETY: dest range checked (R3 + bounds + alignment), source
            // is SRAM (checked above), IRQs masked per session, leaf in RAM.
            let raw =
                cortex_m::interrupt::free(|_| unsafe { ram_cf_program(chunk_addr, ptr, units) });
            if raw != STATUS_OK {
                return Err(FlashError::Program { raw });
            }
            chunk_addr += units * FLASH_WRITE_UNIT;
        }
        Ok(())
    }

    /// Read-back verify: compare code flash at `addr` against `expected`
    /// with volatile reads. Fails with the first mismatching absolute
    /// address.
    pub fn verify(&self, addr: u32, expected: &[u8]) -> Result<(), FlashError> {
        let len = u32::try_from(expected.len()).map_err(|_| FlashError::OutOfBounds)?;
        let end = addr.checked_add(len).ok_or(FlashError::OutOfBounds)?;
        if end > CODE_FLASH_SIZE {
            return Err(FlashError::OutOfBounds);
        }
        for (i, byte) in expected.iter().enumerate() {
            let flash_addr = addr as usize + i;
            // SAFETY: bounds-checked address inside memory-mapped code
            // flash; volatile raw-pointer read (never a reference near
            // address 0 — SPEC §9.1.7).
            let read = unsafe { (flash_addr as *const u8).read_volatile() };
            if read != *byte {
                // Lossless: flash_addr < CODE_FLASH_SIZE fits u32.
                return Err(FlashError::VerifyMismatch {
                    addr: flash_addr as u32,
                });
            }
        }
        Ok(())
    }

    /// Program-then-verify in one call (per-block usage: program the window
    /// buffer, then prove flash matches it before ACKing the host).
    pub fn program_verified(&mut self, addr: u32, data: &[u8]) -> Result<(), FlashError> {
        self.program(addr, data)?;
        self.verify(addr, data)
    }
}

// ---------------------------------------------------------------------------
// embedded-storage traits (M2 DFU / embassy-boot reuse seam, plan §2)
// ---------------------------------------------------------------------------

impl ErrorType for FlashLp {
    type Error = FlashError;
}

impl ReadNorFlash for FlashLp {
    const READ_SIZE: usize = 1;

    /// Volatile code-flash read; `offset` is the absolute address (code
    /// flash is mapped at 0). Reads may span the whole 256 KB, including the
    /// bootloader region — only P/E is R3-guarded.
    fn read(&mut self, offset: u32, bytes: &mut [u8]) -> Result<(), Self::Error> {
        let len = u32::try_from(bytes.len()).map_err(|_| FlashError::OutOfBounds)?;
        let end = offset.checked_add(len).ok_or(FlashError::OutOfBounds)?;
        if end > CODE_FLASH_SIZE {
            return Err(FlashError::OutOfBounds);
        }
        for (i, byte) in bytes.iter_mut().enumerate() {
            let src = offset as usize + i;
            // SAFETY: bounds-checked memory-mapped code-flash address;
            // volatile raw-pointer read, no reference to address 0
            // (SPEC §9.1.7).
            *byte = unsafe { (src as *const u8).read_volatile() };
        }
        Ok(())
    }

    fn capacity(&self) -> usize {
        CODE_FLASH_SIZE as usize
    }
}

impl NorFlash for FlashLp {
    const WRITE_SIZE: usize = FLASH_WRITE_UNIT as usize;
    const ERASE_SIZE: usize = FLASH_BLOCK_SIZE as usize;

    /// Erase `[from, to)` — both 2 KB aligned, inside the app+metadata
    /// region. Each block is its own IRQ-masked critical section.
    fn erase(&mut self, from: u32, to: u32) -> Result<(), Self::Error> {
        if from > to {
            return Err(FlashError::OutOfBounds);
        }
        if !from.is_multiple_of(FLASH_BLOCK_SIZE) || !to.is_multiple_of(FLASH_BLOCK_SIZE) {
            return Err(FlashError::NotAligned);
        }
        let mut block = from;
        while block < to {
            self.erase_block(block)?;
            block += FLASH_BLOCK_SIZE;
        }
        Ok(())
    }

    fn write(&mut self, offset: u32, bytes: &[u8]) -> Result<(), Self::Error> {
        self.program(offset, bytes)
    }
}
