//! Embassy tasks — one module per independent activity (rules/embassy.md).
//!
//! `usb_dfu` is the U2.2 DFU class + task seam; U2.3 (combined parked mode)
//! spawns it alongside `can_update` and `led` (docs/plan.md §6).

pub mod can_update;
pub mod led;
pub mod usb_dfu;
