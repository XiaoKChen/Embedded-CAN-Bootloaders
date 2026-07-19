//! LGPL-free application HAL for the Renesas **R7FA4M1AB** (Arduino UNO R4
//! Minima), built from scratch and licensed MIT OR Apache-2.0
//! (docs/plan-m3-hal.md §1.1 — the whole reason M3 exists: the community RA
//! HALs are LGPL and cannot ship in this repo's MIT/Apache firmware).
//!
//! # What this crate is
//!
//! A peripheral-driver surface layered on [`ra4m1_embassy`] (the shared chip
//! base: HOCO 48 MHz clock tree, PWPR-locked PFS pin-mux, the GPT320
//! `embassy-time` driver, VBTBKR, and the `device.x`/`__INTERRUPTS`
//! interrupt-vector mechanism). This crate adds GPIO, timers/PWM, serial
//! buses, and analog on top; it does **not** re-implement any of the base.
//!
//! # Two binding contracts (rules/embassy.md, rules/embedded.md)
//!
//! - **Traits at the boundary.** Every peripheral exposes `embedded-hal` 1.0
//!   (blocking) and, where a real completion event exists, `embedded-hal-async`
//!   — plus `embedded-io`/`-async` for serial — so drivers are portable and
//!   host-mockable against `embedded-hal-mock`. Concrete register types stay
//!   inside each module.
//! - **Exactly one `embassy-time` driver per binary — the base crate's
//!   GPT320 one.** This crate ships **no** second `time_driver_impl!`. An app
//!   gets time by calling [`init_time`] (re-exported below) after
//!   [`clock::init_hoco_48mhz`]; the HAL's delay types (later phase) are thin
//!   wrappers over `embassy-time`, never a new time source. Consequence: GPT
//!   channel 0 (GPT320) is reserved for the time base and is never offered as
//!   an app PWM/capture channel ([`gpt`], [`interrupt::TIME_DRIVER_SLOT`]).
//!
//! # Interrupts
//!
//! Async peripherals bind ICU/NVIC lines through [`interrupt::bind_interrupts`]
//! against the base crate's weak `PROVIDE(IELn = DefaultHandler)` aliases —
//! the same strong-`#[no_mangle]`-over-weak mechanism the base time driver
//! (IEL0) and the bootloader's USBFS driver (IEL1) already use on silicon. The
//! central slot/priority table lives in [`interrupt`] and cross-links the
//! base crate's IEL0 reservation.
//!
//! # Layering (do not depend on this from the bootloader)
//!
//! The bootloader depends on [`ra4m1_embassy`] and its own register drivers,
//! **never** on this crate (plan §1.2) — its 32 KB size gate stays safe by
//! construction. Apps and this crate's `examples/` depend on the HAL, which
//! depends on the base, so any app binary has exactly one copy of the
//! clocks/PFS/time-driver and exactly one `embassy-time` driver.
//!
//! # Module tree (declared complete at M3.0, plan §3/§6 shared-file rule)
//!
//! Later phase units fill in each stub module's body but never touch this
//! file. Implemented at M3.0: [`clock`], [`interrupt`], [`gpio`]. Stubs
//! awaiting their phase: [`gpt`] (M3.2), [`sci`]/[`spi`]/[`i2c`] (M3.3),
//! [`adc`]/[`dac`] (M3.4).

#![no_std]

pub mod adc;
pub mod clock;
pub mod dac;
pub mod error;
pub mod gpio;
pub mod gpt;
pub mod i2c;
pub mod interrupt;
pub mod sci;
pub mod spi;

// --- Base re-exports (ergonomics; no base-crate edit required) ---------------
// An app pulls the time base through the HAL without naming `ra4m1_embassy`
// directly. This resolves plan §9 Q2 with ZERO base-crate churn: the HAL
// re-exports the base driver's `init`, so no `pub use` was added to
// `ra4m1-embassy/src/lib.rs`.

/// Bring up the shared GPT320 `embassy-time` driver (the single time driver
/// for any binary, plan §1.3). Call once, after [`clock::init_hoco_48mhz`]
/// and before the executor starts. Re-export of
/// [`ra4m1_embassy::time_driver::init`].
pub use ra4m1_embassy::time_driver::init as init_time;

/// The shared PWPR-locked PFS pin-mux helpers, re-exported for drivers and
/// apps that route peripheral functions. Re-export of [`ra4m1_embassy::pfs`].
pub use ra4m1_embassy::pfs;
