//! GPIO — typed pins over the RA4M1 PORT block, muxed through the base
//! crate's PWPR-locked PFS helpers (M3.1, plan §4.2/§8).
//!
//! # Design
//!
//! - **Pin mode** (direction / peripheral-mux) is configured **once** at
//!   construction through [`ra4m1_embassy::pfs`] — the PWPR write-protect
//!   protocol lives there and is not re-implemented here (plan §8 reuse
//!   ledger).
//! - **Runtime** set / clear / read touch only the PORT block, which is
//!   **not** PWPR-protected, so they are single register writes with no
//!   unlock dance. Set/clear use `POSR`/`PORR` (per-bit *set-only* / *reset-
//!   only* registers), so two pins on the same port never race through a
//!   read-modify-write: each write affects only its own bit and leaves the
//!   rest "no affect" (R01UM0007, I/O Ports chapter). This is what lets the
//!   driver hand each pin out as an independent owned value.
//!
//! # Register coverage (spike S-B)
//!
//! The `ra4m1` 0.2 PAC types the PORT block (`PODR`/`PDR`/`PIDR`/`PORR`/`POSR`)
//! fully. All ten ports share the `port0::RegisterBlock` layout at
//! `0x4004_0000 + 0x20 * port`, so this module reaches a port by that one
//! documented computed base (the same contained-address discipline as
//! `pfs.rs`) and then uses the PAC's **typed** register accessors — no raw
//! field pokes.
//!
//! # Errors
//!
//! Blocking GPIO cannot fail on this hardware, so the `embedded-hal`
//! `ErrorType::Error` is [`Infallible`] (rules/rust.md / [`crate::error`]).
//!
//! # Later phase
//!
//! Async edge/level wait (`embedded-hal-async` `digital::Wait`) is spike S-D /
//! a later unit: it needs an ICU pin-event routed to a waker via
//! [`crate::bind_interrupts`]. Not implemented at M3.0 — blocking pins only.

use core::convert::Infallible;

use embedded_hal::digital::{ErrorType, InputPin, OutputPin, StatefulOutputPin};
use ra4m1::{PFS, PMISC};

use ra4m1_embassy::pfs;

/// PORT register-block base address (R01UM0007, I/O Ports register map).
const PORT_BASE: usize = 0x4004_0000;
/// Stride between consecutive PORTn register blocks.
const PORT_STRIDE: usize = 0x20;
/// Highest port index on the RA4M1 (ports 0..=9).
const PORT_MAX: u8 = 9;
/// Highest pin index within a port (pins 0..=15).
const PIN_MAX: u8 = 15;

/// Initial output drive level for [`Output::new`].
#[derive(Clone, Copy, Debug, PartialEq, Eq, defmt::Format)]
pub enum Level {
    /// Drive the pin low (0 V).
    Low,
    /// Drive the pin high (VCC).
    High,
}

/// The PORT register block for `port`, via its documented computed base.
///
/// All PORTn share the `port0::RegisterBlock` layout; only the base differs.
fn port_regs(port: u8) -> &'static ra4m1::port0::RegisterBlock {
    debug_assert!(port <= PORT_MAX, "port index out of range (0..=9)");
    // SAFETY: address = documented PORT base + 0x20 * port, which lands on the
    // PORTn block for port <= 9 (R01UM0007, I/O Ports register map). Shared-
    // reference MMIO is the PAC's own model. Distinct pins use distinct POSR/
    // PORR bits (set-only / reset-only, no RMW), so concurrent access to the
    // same block through two such references does not race.
    unsafe { &*((PORT_BASE + PORT_STRIDE * port as usize) as *const ra4m1::port0::RegisterBlock) }
}

/// A GPIO pin configured as a push-pull output.
///
/// Constructed from the port/pin coordinates plus the `PFS`/`PMISC` singletons
/// (which prove the caller owns pin-mux, the same ownership proof
/// [`ra4m1_embassy::pfs`] uses). Move the value into the task that drives the
/// pin; construct each physical pin exactly once.
pub struct Output {
    port: u8,
    /// `1 << pin` — precomputed bit mask into the port registers.
    mask: u16,
}

impl Output {
    /// Configure `Pport_pin` as a push-pull GPIO output driving `initial`.
    ///
    /// `port` must be 0..=9 and `pin` 0..=15 (debug-asserted; out-of-range is a
    /// board-wiring bug, not a runtime condition).
    #[must_use]
    pub fn new(pfs_reg: &PFS, pmisc: &PMISC, port: u8, pin: u8, initial: Level) -> Self {
        debug_assert!(port <= PORT_MAX && pin <= PIN_MAX);
        pfs::set_gpio_output(pfs_reg, pmisc, port, pin, matches!(initial, Level::High));
        Self {
            port,
            mask: 1 << pin,
        }
    }
}

impl ErrorType for Output {
    type Error = Infallible;
}

impl OutputPin for Output {
    fn set_high(&mut self) -> Result<(), Infallible> {
        // POSR: write 1 to set the pin high; 0 bits have no effect.
        // SAFETY: mask = 1<<pin with pin <= 15 by construction; POSR is
        // set-only (write-1-to-set), so there is no cross-pin RMW race.
        port_regs(self.port)
            .posr()
            .write(|w| unsafe { w.bits(self.mask) });
        Ok(())
    }

    fn set_low(&mut self) -> Result<(), Infallible> {
        // PORR: write 1 to drive the pin low; 0 bits have no effect.
        // SAFETY: mask = 1<<pin with pin <= 15 by construction; PORR is
        // reset-only (write-1-to-reset), so there is no cross-pin RMW race.
        port_regs(self.port)
            .porr()
            .write(|w| unsafe { w.bits(self.mask) });
        Ok(())
    }
}

impl StatefulOutputPin for Output {
    fn is_set_high(&mut self) -> Result<bool, Infallible> {
        // PODR reflects the latched output level POSR/PORR drive.
        Ok(port_regs(self.port).podr().read().podr().bits() & self.mask != 0)
    }

    fn is_set_low(&mut self) -> Result<bool, Infallible> {
        Ok(!self.is_set_high()?)
    }
}

/// A GPIO pin configured as a digital input (floating).
///
/// M3.0 exposes a floating input (PmnPFS reset state: input, no pull). An
/// internal pull-up/down selector needs a `PmnPFS.PCR` write, which the base
/// `pfs` module does not yet expose; adding it is a base-crate change (flagged,
/// not made at M3.0). Use an external resistor, or wait for that helper.
pub struct Input {
    port: u8,
    mask: u16,
}

impl Input {
    /// Configure `Pport_pin` as a floating GPIO input.
    #[must_use]
    pub fn new(pfs_reg: &PFS, pmisc: &PMISC, port: u8, pin: u8) -> Self {
        debug_assert!(port <= PORT_MAX && pin <= PIN_MAX);
        pfs::reset_to_gpio_input(pfs_reg, pmisc, port, pin);
        Self {
            port,
            mask: 1 << pin,
        }
    }
}

impl ErrorType for Input {
    type Error = Infallible;
}

impl InputPin for Input {
    fn is_high(&mut self) -> Result<bool, Infallible> {
        Ok(port_regs(self.port).pidr().read().pidr().bits() & self.mask != 0)
    }

    fn is_low(&mut self) -> Result<bool, Infallible> {
        Ok(!self.is_high()?)
    }
}
