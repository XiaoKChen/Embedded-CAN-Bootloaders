//! Pure bootloader logic: memory map, CRC-32, image-metadata validation, the CAN update
//! protocol engine and DFU config-block parsing.
//!
//! Zero PAC/HAL/executor dependencies (docs/plan.md §3, rules/rust.md): `no_std` in firmware
//! builds and `std`-available under `cargo test`, so every module host-tests without hardware.
//! The module tree is declared complete here (docs/plan.md §6 shared-file rule); later units
//! fill in module bodies but never touch this file.
//!
//! Owns:        the crate's module tree.
//! Not here:    hardware access — callers reach flash only through the `ImageRead` trait (see
//!              `image_meta`), never through slices conjured at a fixed address (SPEC §9.1.7).
//! Depends on:  nothing outside `core`.
//! See also:    `bootloader/` (the firmware that consumes this crate).

#![cfg_attr(not(test), no_std)]

pub mod crc32;
pub mod dfu_config;
pub mod image_meta;
pub mod memory_map;
pub mod protocol;

#[cfg(test)]
mod tests {
    /// Placeholder proving the host-test harness runs; each module carries
    /// its own real tests once its owning unit lands.
    #[test]
    fn crate_links_on_host() {
        assert_eq!(2 + 2, 4);
    }
}
