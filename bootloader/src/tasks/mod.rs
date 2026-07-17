//! Embassy tasks — one module per independent activity (rules/embassy.md).
//!
//! `usb_dfu.rs` is Milestone 2 (USB DFU) and is added by U2.2 — not
//! declared here yet (docs/plan.md §6).

pub mod can_update;
pub mod led;
