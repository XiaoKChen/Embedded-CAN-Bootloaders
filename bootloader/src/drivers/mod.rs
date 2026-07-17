//! Device drivers, register-level against the `ra4m1` PAC (no Embassy HAL
//! exists for RA — ECO §1). One module per peripheral concern
//! (rules/embedded.md).
//!
//! `usbfs.rs` is Milestone 2 (USB DFU) and is added by U2.1 — not declared
//! here yet (docs/plan.md §6).

pub mod can0;
pub mod flash_lp;
pub mod usb_rail;
