//! CAN-transceiver-rail power-up via the on-chip USB regulator (SPEC §5.3,
//! R33) — a power fix, not USB.
//!
//! WHY: custom boards feed the external CAN transceiver / level-shifter
//! path from the RA4M1's USB regulator rail (VCC_USB). Until that
//! regulator is on, CRX0 never shows recessive: the controller reaches
//! operate mode and TRMREQ sets, but TEC stays frozen at 0 — the board is
//! deaf and mute on every ID (SPEC §9.1.1, bench-confirmed via the C
//! bootloader's one-boot staircase diagnostic on a real bus). The USB-DFU
//! path always powered the rail as a side effect of `tud_init()`, which is
//! why only the CAN-only magic-entry path was silent. Call this BEFORE any
//! CAN register access on CAN-update-mode entry. Harmless on boards
//! without the dependency: the LDO merely energizes an unused VCC_USB pad.
//!
//! Faithful port of `can_xcvr_usb_rail_power_on`
//! (`main:vendor/arduino-renesas-bootloader/src/main.c`) — only the
//! power-relevant writes of the proven DFU bring-up, in its order, with no
//! USB stack. Registers: R01UM0007 (RA4M1 HW manual), USBFS chapter
//! (SYSCFG, USBMC), Low Power Modes chapter (MSTPCRB), and the Clock
//! Generation Circuit chapter (USBCKCR, via `clocks::init_usb_clock_48mhz`).
use ra4m1_embassy::clocks;

/// SCKE read-back poll bound — C parity (`CAN_USB_RAIL_SCKE_TIMEOUT_LOOPS`).
const SCKE_READBACK_POLLS: u32 = 1000;
/// Regulator soak before the first CAN access. No VDCEN ramp figure exists
/// in FSP or the HW manual; 5 ms is an engineering margin comfortably
/// above typical LDO start-up plus downstream rail charge (SPEC §9.4).
const RAIL_SOAK_MS: u32 = 5;
/// The soak in CPU cycles at ICLK 48 MHz (`cortex_m::asm::delay` waits at
/// least this many cycles).
const RAIL_SOAK_CYCLES: u32 = clocks::ICLK_HZ / 1_000 * RAIL_SOAK_MS;

/// Power the VCC_USB rail (blocking, ~5 ms total): USBFS module-start →
/// SYSCFG.SCKE with a bounded read-back wait → USBMC.VDCEN → 5 ms soak.
///
/// Cycle-counted delays only — no `embassy-time` — so it is callable
/// pre-executor (spike S4). When U1.4 calls it at CAN-task start it is a
/// one-shot, bounded bring-up delay: a documented exception to the
/// no-blocking rule alongside docs/plan.md §4's flash-P/E and TX-poll
/// exceptions. Cannot fail: an SCKE timeout falls through deliberately
/// (SPEC §5.3) — worst case the VDCEN write is lost and the board parks
/// serving CAN degraded, never dead.
pub fn power_up_transceiver_rail() {
    // SAFETY: transient PAC-singleton re-acquisition for MSTP + USBFS.
    // MSTPCRB's read-modify-write cannot race: every MSTPCRB user (this
    // function, drivers::can0) runs sequentially in the same boot/task
    // context and no ISR touches it. Nothing else owns USBFS until M2's
    // usbfs.rs, which depends on this power-up having run anyway.
    let p = unsafe { ra4m1::Peripherals::steal() };

    // 0. Point UCLK at HOCO 48 MHz BEFORE the module comes out of module-stop
    //    and before SCKE gates the USB clock in — otherwise the USBFS
    //    operational clock domain (PHY, INTSTS0, ...) has no clock and every
    //    operational-register access faults. USBCKCR resets to PLL, which is
    //    disabled on this HOCO-only clock plan (SPEC §5.2 / clocks.rs).
    clocks::init_usb_clock_48mhz(&p.SYSTEM);

    // 1. Release USBFS module-stop (MSTPCRB bit 11, SPEC §5.3 step 1); the
    //    dummy read-back completes the write before the first USBFS access.
    p.MSTP.mstpcrb.modify(|_, w| w.mstpb11()._0());
    let _ = p.MSTP.mstpcrb.read();

    // 2. SYSCFG.SCKE = 1 — USB clock supply; it gates the other USBFS
    //    registers, so wait (bounded) until it reads back set (SPEC §5.3
    //    step 2).
    p.USBFS.syscfg.modify(|_, w| w.scke()._1());
    for _ in 0..SCKE_READBACK_POLLS {
        if p.USBFS.syscfg.read().scke().is_1() {
            break;
        }
    }
    // A timeout above falls through deliberately (see the function doc).

    // 3. USBMC.VDCEN = 1 — USB LDO regulator ON (SPEC §5.3 step 3).
    p.USBFS.usbmc.modify(|_, w| w.vdcen()._1());

    // 4. Let the regulator and the downstream transceiver rail charge
    //    before the first CAN access (SPEC §5.3 step 4).
    cortex_m::asm::delay(RAIL_SOAK_CYCLES);
}
