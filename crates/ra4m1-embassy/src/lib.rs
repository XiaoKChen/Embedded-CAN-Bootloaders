//! Shared RA4M1 chip runtime: clock bring-up, the `embassy-time` driver, VBTBKR access, PFS
//! pin-function writes and (behind `usb`) the USBFS driver.
//!
//! No Embassy HAL exists for RA (ECO §1) — this crate is the HAL seam for this chip, consumed
//! by both `bootloader` and `demo-app`. PAC-level and contained per rules/embedded.md: one
//! module per peripheral concern, every `unsafe` block carries a `// SAFETY:` comment with the
//! RA4M1 hardware manual (R01UM0007) section it implements. The module tree is declared
//! complete here (docs/plan.md §6 shared-file rule).
//!
//! Owns:        the chip-level register access shared by the firmware binaries.
//! Not here:    board wiring and interrupt vector bindings (the binaries; see `usb`).
//! Invariants:  target-only — it touches real registers, so it builds solely for
//!              `thumbv7em-none-eabihf` and has no host tests.
//! Depends on:  the `ra4m1` PAC, `cortex-m`, `embassy-time-driver`.
//! See also:    `bootloader/`, `demo-app/`.

#![no_std]

pub mod clocks;
pub mod pfs;
pub mod time_driver;
pub mod vbtbkr;

/// RA4M1 USBFS device driver (`embassy-usb-driver`), behind the `usb` feature.
/// The consuming binary binds the `IEL1` vector to [`usb::on_interrupt`].
#[cfg(feature = "usb")]
pub mod usb;
