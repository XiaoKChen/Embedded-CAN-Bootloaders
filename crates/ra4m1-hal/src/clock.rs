//! Clock query/config surface over the base crate's clock tree.
//!
//! This module **wraps, never duplicates**, [`ra4m1_embassy::clocks`]: the
//! HOCO 48 MHz bring-up and every rate constant live in the base crate (plan
//! §8 reuse ledger). Here they are re-exported and given a by-name query API
//! so driver code can derive timing (baud divisors, GPT prescale, ADC
//! sampling clock) from the exact domain that feeds each peripheral —
//! e.g. SCI baud is off PCLKB, GPT counts off PCLKD — without hard-coding a
//! frequency at the call site (rules/principles.md "constants are centralized").
//!
//! # Configuration
//!
//! There is one clock configuration on this board: HOCO 48 MHz with the fixed
//! divider tree ([`init_hoco_48mhz`]), plus the USB UCLK source select
//! ([`init_usb_clock_48mhz`]). Both are re-exports of the base crate's proven,
//! on-silicon bring-up. The HAL adds no alternate clock trees.
//!
//! # Per-peripheral clock gating (module-stop)
//!
//! Enabling a peripheral's clock is a write to an `MSTPCR{A,B,C,D}` bit
//! (R01UM0007, Low Power Modes chapter) and is **owned by each peripheral
//! driver's `init`**, co-located with the register bring-up that needs it —
//! exactly as the base time driver clears `MSTPCRD.MSTPD5` for GPT320. It is
//! deliberately not centralized here: a generic `enable(peripheral)` would
//! need a full peripheral→MSTP-bit map that only makes sense once the drivers
//! that consume it exist. This module answers "how fast does clock X run?";
//! each driver answers "is my module clocked?".

pub use ra4m1_embassy::clocks::{init_hoco_48mhz, init_usb_clock_48mhz};
pub use ra4m1_embassy::clocks::{
    FCLK_HZ, ICLK_HZ, PCLKA_HZ, PCLKB_HZ, PCLKC_HZ, PCLKD_HZ, USBCLK_HZ,
};

/// Every clock domain the R7FA4M1AB exposes to peripherals, so app/driver
/// code can query a rate by the domain name a datasheet uses rather than a
/// bare constant. Values are the post-[`init_hoco_48mhz`] rates.
#[derive(Clone, Copy, Debug, PartialEq, Eq, defmt::Format)]
pub enum Clock {
    /// System / CPU clock (ICLK). 48 MHz.
    Iclk,
    /// Peripheral module clock A (PCLKA) — GPT, SPI, SCI(SPI/async base).
    /// 48 MHz.
    PclkA,
    /// Peripheral module clock B (PCLKB) — SCI (UART baud), IIC, CAN.
    /// 24 MHz.
    PclkB,
    /// Peripheral module clock C (PCLKC). 48 MHz.
    PclkC,
    /// Peripheral module clock D (PCLKD) — GPT count clock (the time driver
    /// runs GPT320 off this ÷16). 48 MHz.
    PclkD,
    /// Flash interface clock (FCLK). 24 MHz.
    Fclk,
    /// USB clock (UCLK), valid only after [`init_usb_clock_48mhz`]. 48 MHz.
    Usb,
}

impl Clock {
    /// The domain's frequency in Hz after [`init_hoco_48mhz`] (and, for
    /// [`Clock::Usb`], after [`init_usb_clock_48mhz`]).
    #[must_use]
    pub const fn hz(self) -> u32 {
        match self {
            Clock::Iclk => ICLK_HZ,
            Clock::PclkA => PCLKA_HZ,
            Clock::PclkB => PCLKB_HZ,
            Clock::PclkC => PCLKC_HZ,
            Clock::PclkD => PCLKD_HZ,
            Clock::Fclk => FCLK_HZ,
            Clock::Usb => USBCLK_HZ,
        }
    }
}
