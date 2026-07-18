//! Device drivers, register-level against the `ra4m1` PAC (no Embassy HAL
//! exists for RA — ECO §1). One module per peripheral concern
//! (rules/embedded.md).
//!
//! `usbfs.rs` (U2.1) is the Milestone-2 `embassy-usb-driver` for the RA4M1
//! USBFS device controller (docs/plan.md §6); U2.3 wires it into parked mode.

pub mod can0;
pub mod flash_lp;
pub mod usb_rail;
pub mod usbfs;
