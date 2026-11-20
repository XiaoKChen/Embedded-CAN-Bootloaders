//! Embassy tasks, one module per independent activity.
//!
//! `usb_dfu` is the U2.2 DFU class + task seam; U2.3 (combined parked mode) spawns it alongside
//! `can_update` and `led` (docs/plan.md §6).
//!
//! Not here:  register-level peripheral access (`drivers/`).
//! See also:  rules/embassy.md; `drivers/mod.rs`.

pub mod can_update;
pub mod led;
pub mod usb_dfu;
