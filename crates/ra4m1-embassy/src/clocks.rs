//! HOCO 48 MHz clock bring-up: ICLK 48 / PCLKA 48 / PCLKB 24 / FCLK 24
//! (SPEC §5.2).
//!
//! Mirrors the FSP `bsp_clock_init` path the C bootloader shipped with,
//! using the UNO R4 values from `vendor/tinyusb/.../uno_r4/bsp_clock_cfg.h`
//! (on `main`): no crystal, HOCO 48 MHz as the system clock, dividers
//! ICLK /1, PCLKA /1, PCLKB /2, PCLKC /1, PCLKD /1, FCLK /2. PCLKA:PCLKB
//! stays 2:1, which CAN0 requires on this part (SPEC §5.2).
//!
//! Registers: R01UM0007 (RA4M1 HW manual), Clock Generation Circuit chapter
//! (SCKDIVCR, SCKSCR, HOCOCR, OSCSF), MEMWAIT (Memory Wait Cycle Control),
//! and the Register Write Protection chapter (PRCR).

use ra4m1::SYSTEM;

/// PRCR protection key: R01UM0007, Register Write Protection chapter — the
/// upper byte of every PRCR write must be 0xA5 or the write is ignored.
/// Writing the bare key re-locks everything (PRC0/PRC1/PRC3 = 0).
pub(crate) const PRCR_KEY: u16 = 0xA500;
/// PRCR.PRC0 — unlocks the clock-generation registers (SCKDIVCR, SCKSCR,
/// HOCOCR, MEMWAIT, ...).
pub(crate) const PRCR_PRC0_CLOCKS: u16 = 0x0001;
/// PRCR.PRC1 — unlocks the low-power / battery-backup registers, including
/// VBTBKR (used by `crate::vbtbkr`; same key/lock scheme, kept here so the
/// PRCR protocol lives in one place).
pub(crate) const PRCR_PRC1_LPM_VBT: u16 = 0x0002;

/// System (CPU) clock after [`init_hoco_48mhz`], in Hz.
pub const ICLK_HZ: u32 = 48_000_000;
/// Peripheral module clock A after [`init_hoco_48mhz`], in Hz.
pub const PCLKA_HZ: u32 = 48_000_000;
/// Peripheral module clock B (CAN0 source clock, `BCR.CCLKS = 0`) after
/// [`init_hoco_48mhz`], in Hz.
pub const PCLKB_HZ: u32 = 24_000_000;
/// Peripheral module clock C after [`init_hoco_48mhz`], in Hz.
pub const PCLKC_HZ: u32 = 48_000_000;
/// Peripheral module clock D after [`init_hoco_48mhz`], in Hz.
pub const PCLKD_HZ: u32 = 48_000_000;
/// Flash interface clock after [`init_hoco_48mhz`], in Hz.
pub const FCLK_HZ: u32 = 24_000_000;

/// Bring the chip from its reset clock (MOCO 8 MHz) to HOCO 48 MHz with the
/// divider tree above. Call exactly once, before anything derives timing
/// from the clock tree.
///
/// Blocking (oscillation-stabilization poll) — allowed here because no
/// executor is running yet (docs/plan.md §4). The HOCO ready flag has no
/// documented failure mode short of a silicon defect; FSP waits unbounded
/// on it too.
pub fn init_hoco_48mhz(system: &SYSTEM) {
    // SAFETY: whole-register PRCR write of the documented key + PRC0 value
    // (R01UM0007, Register Write Protection). Unlocks the CGC registers
    // written below; re-locked at the end of this function.
    system
        .prcr
        .write(|w| unsafe { w.bits(PRCR_KEY | PRCR_PRC0_CLOCKS) });

    // HOCO normally auto-starts at 48 MHz via the option bytes (OFS1
    // HOCOEN = 0, HOCOFRQ1 = 0b100 — bootloader/src/ofs.rs). HCSTP = 0 also
    // covers entry with the HOCO stopped.
    system.hococr.write(|w| w.hcstp()._0());
    while system.oscsf.read().hocosf().is_0() {}

    // One flash-read wait state, required before ICLK exceeds 32 MHz
    // (R01UM0007 MEMWAIT: clearing it is prohibited while ICLK > 32 MHz;
    // must be set beforehand, in high-speed mode — the reset default).
    system.memwait.write(|w| w.memwait()._1());

    // Dividers before the source switch so no domain overshoots its limit
    // when SCKSCR flips to the faster oscillator.
    system.sckdivcr.write(|w| {
        w.ick()._000(); // ICLK  /1 -> 48 MHz
        w.pcka()._000(); // PCLKA /1 -> 48 MHz
        w.pckb()._001(); // PCLKB /2 -> 24 MHz
        w.pckc()._000(); // PCLKC /1 -> 48 MHz
        w.pckd()._000(); // PCLKD /1 -> 48 MHz
        w.fck()._001() // FCLK  /2 -> 24 MHz
    });

    // System clock source = HOCO. Read back so the switch is complete
    // before any caller derives timing from the new tree.
    system.sckscr.write(|w| w.cksel()._000());
    let _ = system.sckscr.read(); // intentional: completion read-back

    // SAFETY: whole-register PRCR write of the documented key (re-lock).
    system.prcr.write(|w| unsafe { w.bits(PRCR_KEY) });
}
