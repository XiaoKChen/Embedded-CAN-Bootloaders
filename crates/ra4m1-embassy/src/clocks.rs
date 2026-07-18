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
/// USB clock (UCLK) after [`init_usb_clock_48mhz`], in Hz. USBFS full-speed
/// requires exactly 48 MHz; UCLK is HOCO undivided on this part.
pub const USBCLK_HZ: u32 = 48_000_000;

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

/// Point the USB clock (UCLK) at HOCO 48 MHz so the USBFS operational clock
/// domain (PHY, INTSTS0, DCPMAXP, ...) is actually clocked. Call once,
/// AFTER [`init_hoco_48mhz`] and BEFORE any USBFS operational-register access
/// (i.e. before the module leaves module-stop / SCKE gates its clock in).
///
/// Precondition: [`init_hoco_48mhz`] has run, so HOCO is oscillating and
/// stable at 48 MHz and the PLL is left disabled (the UNO R4 / RA4M1 clock
/// plan — `vendor/.../uno_r4/bsp_clock_cfg.h` on `main`:
/// `BSP_CFG_UCK_SOURCE = HOCO`, `BSP_CFG_PLL_SOURCE = DISABLED`).
///
/// WHY: `USBCKCR.USBCLKSEL` resets to 0 = PLL. With the PLL disabled (this
/// bootloader runs HOCO only), the reset default leaves UCLK unsourced, so
/// every USBFS operational register faults on access even though `SYSCFG`
/// (in the always-on clock-control domain) reads/writes fine — exactly the
/// bench-observed split (imprecise BusFault in `phy_init_fs` / `hw_enable`).
/// Selecting HOCO gives UCLK a live 48 MHz. USB full-speed needs exactly
/// 48 MHz and HOCO is 48 MHz, so UCLK = HOCO / 1 — this part has no USB
/// clock divider register, the source IS the clock.
///
/// Registers: R01UM0007 (RA4M1 HW manual), Clock Generation Circuit chapter,
/// USBCKCR (USB Clock Control Register, @ SYSTEM+0xD0), field USBCLKSEL
/// (bit 0: 0 = PLL, 1 = HOCO). This part has NO USB-clock switching-request /
/// switching-ready handshake — that USBCKCR variant (USBCKSREQ/USBCKSRDY at
/// SYSTEM+0x74) belongs to other RA families and is absent on the R7FA4M1AB
/// (the `ra4m1` PAC exposes only the single-bit register here), so the source
/// select takes effect with no poll. Non-blocking; callable pre-executor.
pub fn init_usb_clock_48mhz(system: &SYSTEM) {
    // SAFETY: whole-register PRCR write of the documented key + PRC0 value
    // (R01UM0007, Register Write Protection). USBCKCR is a CGC register under
    // PRCR.PRC0; unlock, write the source select, re-lock.
    system
        .prcr
        .write(|w| unsafe { w.bits(PRCR_KEY | PRCR_PRC0_CLOCKS) });

    // UCLK source = HOCO (48 MHz). Reset default is PLL, which is disabled.
    system.usbckcr.write(|w| w.usbclksel()._1());
    let _ = system.usbckcr.read(); // completion read-back before re-lock

    // SAFETY: whole-register PRCR write of the documented key (re-lock).
    system.prcr.write(|w| unsafe { w.bits(PRCR_KEY) });
}
