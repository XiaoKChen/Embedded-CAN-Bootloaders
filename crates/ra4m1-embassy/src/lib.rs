//! Shared RA4M1 chip runtime: clock bring-up, the custom `embassy-time`
//! driver, VBTBKR (backup register) access, and PFS pin-function writes.
//!
//! No Embassy HAL exists for RA (ECO §1) — this crate is the HAL seam for
//! this chip, consumed by both `bootloader` and `demo-app`. PAC-level and
//! contained per rules/embedded.md: one module per peripheral concern, every
//! `unsafe` block carries a `// SAFETY:` comment with the RA4M1 hardware
//! manual (R01UM0007) section it implements.
//!
//! Target-only (no host tests): this crate touches real registers, so it
//! builds solely for `thumbv7em-none-eabihf`.
//!
//! Module tree is declared complete here (docs/plan.md §6 shared-file rule):
//! later units fill in each module's body but never touch this file.

#![no_std]

pub mod clocks;
pub mod pfs;
pub mod time_driver;
pub mod vbtbkr;
