//! DAC12 (12-bit DAC) — blocking channel-0 output (M3.4, plan §4.2/§4.3/§4.5).
//!
//! # What this module implements
//!
//! A **bespoke inherent blocking** API over the RA4M1's single DAC12 channel
//! (channel 0, output pin **DA0 = P014**). [`Dac::set_value`] is a single
//! `DADR0` register write with no completion event to await, so it stays
//! blocking (plan §4.5 — making it async would be theatre). `embedded-hal` has
//! no DAC trait, so no trait is implemented; the inherent API is documented and
//! stable. DAC operations cannot fail on this hardware, so there is **no error
//! enum** ([`crate::error`] convention — the honest type for an infallible path,
//! exactly as [`crate::gpio`] uses `Infallible`).
//!
//! # Register recipe (R01UM0007, 12-Bit D/A Converter chapter; Low Power Modes
//! chapter for MSTP)
//!
//! | Step | Register / bit | Value | Purpose |
//! |---|---|---|---|
//! | Module-start | `MSTP.MSTPCRD.MSTPD20` | 0 | release DAC12 from module-stop |
//! | Sync convert | `DAC12.DAADSCR.DAADST` | 0 | independent DAC (no A/D-sync) |
//! | Reference | `DAC12.DAVREFCR.REF` | 0b001 | AVCC0 / AVSS0 reference |
//! | Format | `DAC12.DADPR.DPSEL` | 0 | right-justified (low 12 bits used) |
//! | Data | `DAC12.DADR0.DADR` | 0..=4095 | conversion value |
//! | Output enable | `DAC12.DACR.DAOE0` | 1 | drive DA0 (P014) |
//!
//! # Pin configuration (ASEL)
//!
//! DA0 = P014 is put into its analog state via the base crate's
//! [`ra4m1_embassy::pfs::set_analog`] (PmnPFS.ASEL = 1, GPIO input path
//! disconnected, no peripheral mux) — the DAC's analog output buffer then drives
//! the pad. ASEL is the analog-mode bit FSP sets for analog pins; it improves
//! isolation/accuracy (the pad would still be driven without it).
//!
//! # Settling time (tDACR)
//!
//! After `DAOE0` is set, R01UM0007 specifies a stabilization wait (tDACR, a few
//! µs) before the output is fully valid. This module does not busy-wait during
//! `new()`; the first meaningful [`Dac::set_value`] plus the natural time before
//! an external load samples it covers the settle in practice. Callers needing a
//! guaranteed-settled first sample should `Timer::after` a few µs after `new()`.

use ra4m1::{DAC12, PFS, PMISC};

use ra4m1_embassy::pfs;

/// DAC12 output pin DA0: port 0, pin 14 (R01UM0007 pin-function map).
const DA0_PORT: u8 = 0;
const DA0_PIN: u8 = 14;
/// The DAC is 12-bit; values are masked to this range (right-justified).
const DAC_MAX: u16 = 0x0FFF;

/// The RA4M1 module-stop register block, via the PAC singleton pointer.
///
/// `MSTPCRD` is shared across peripheral drivers (the base time driver owns
/// `MSTPD5`, this driver `MSTPD20`, the ADC `MSTPD16`); each driver clears only
/// its own bit, at init, before the executor starts — no concurrent access.
fn mstp() -> &'static ra4m1::mstp::RegisterBlock {
    // SAFETY: shared-reference MMIO to the MSTP block — the PAC's own access
    // model (mirrors `interrupt.rs`'s `&*ICU::PTR`). This driver touches only
    // MSTPCRD.MSTPD20; the read-modify-write runs once at init on a single
    // thread, so it does not race the base crate's MSTPD5 bring-up.
    unsafe { &*ra4m1::MSTP::PTR }
}

/// The RA4M1 12-bit DAC, channel 0 (output DA0 = P014).
///
/// Constructed once from the [`DAC12`] PAC singleton (proving exclusive
/// ownership) plus the `PFS`/`PMISC` singletons for pin-mux. Move the value into
/// the task that drives the output.
pub struct Dac {
    dac: DAC12,
}

impl Dac {
    /// Bring up DAC12 channel 0: release module-stop, select the AVCC0/AVSS0
    /// reference and right-justified format, put DA0 (P014) into analog mode
    /// (PmnPFS.ASEL), and enable the output driving 0.
    ///
    /// `pfs_reg`/`pmisc` are the pin-mux singletons (the same ownership proof
    /// [`ra4m1_embassy::pfs`] requires); consumed by reference.
    #[must_use]
    pub fn new(dac: DAC12, pfs_reg: &PFS, pmisc: &PMISC) -> Self {
        // Module-start DAC12 (MSTPD20 = 0), read back so the module is clocked
        // before its registers are touched (R01UM0007, Low Power Modes chapter).
        mstp().mstpcrd.modify(|_, w| w.mstpd20()._0());
        let _ = mstp().mstpcrd.read();

        // DA0 = P014 to analog mode (PmnPFS.ASEL; input path off, no peripheral).
        pfs::set_analog(pfs_reg, pmisc, DA0_PORT, DA0_PIN);

        // Independent DAC (no A/D synchronous conversion).
        dac.daadscr.modify(|_, w| w.daadst()._0());
        // Reference = AVCC0 / AVSS0.
        dac.davrefcr.modify(|_, w| w.ref_()._001());
        // Right-justified data format (low 12 bits of DADR0 are the value).
        dac.dadpr.modify(|_, w| w.dpsel()._0());
        // Start from 0, then enable the channel-0 output on DA0.
        dac.dadr0.write(|w| unsafe { w.dadr().bits(0) });
        dac.dacr.modify(|_, w| w.daoe0()._1());

        Self { dac }
    }

    /// Write a 12-bit conversion value (0..=4095). Values above 4095 are
    /// masked to 12 bits (the register's high 4 bits are fixed to 0 in
    /// right-justified format). Single register write; returns immediately.
    pub fn set_value(&mut self, value: u16) {
        self.dac
            .dadr0
            .write(|w| unsafe { w.dadr().bits(value & DAC_MAX) });
    }

    /// Enable the DA0 output buffer (`DACR.DAOE0 = 1`). Idempotent; `new()`
    /// already enables the channel.
    pub fn enable(&mut self) {
        self.dac.dacr.modify(|_, w| w.daoe0()._1());
    }

    /// Disable the DA0 output buffer (`DACR.DAOE0 = 0`); the pin goes
    /// high-impedance. The conversion value in `DADR0` is retained.
    pub fn disable(&mut self) {
        self.dac.dacr.modify(|_, w| w.daoe0()._0());
    }
}
