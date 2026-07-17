//! Code-flash partition map for the R7FA4M1AB (256 KB code flash, 128 × 2 KB
//! erase blocks), per the locked decisions in docs/plan.md §1.2 — these values
//! **supersede SPEC §1.1** (bootloader grew 16 KB → 32 KB, `APP_BASE` moved to
//! `0x8000`, app slot shrank to 111 blocks).
//!
//! Single source of truth for partition constants; the linker scripts
//! (`bootloader/memory.x`, `demo-app/memory.x`) must agree with these values.
//! The const-asserts at the bottom prove the partitions tile the flash exactly
//! and stay erase-block aligned — any inconsistent edit fails to compile.

/// Code flash is memory-mapped at address 0 (SPEC §1.1).
pub const CODE_FLASH_BASE: u32 = 0x0000_0000;

/// Total code flash: 256 KB (SPEC §1.1).
pub const CODE_FLASH_SIZE: u32 = 0x0004_0000;

/// Erase-block size in bytes (SPEC §1.1 `FLASH_BLOCK_SIZE`).
pub const FLASH_BLOCK_SIZE: u32 = 2048;

/// Code-flash write unit in bytes — programs are 8-byte-aligned multiples
/// (SPEC §1.1 `FLASH_WRITE_UNIT`).
pub const FLASH_WRITE_UNIT: u32 = 8;

/// Bootloader region size: 32 KB, blocks 0–15 (plan §1.2; supersedes SPEC's
/// 16 KB). Also the runtime never-write-below floor (R3, tightened).
pub const BOOT_SIZE: u32 = 0x8000;

/// App slot base and vector-table address: `0x0000_8000` (plan §1.2;
/// supersedes R5's `0x4000`). VTOR/MSP/entry are taken from here.
pub const APP_BASE: u32 = CODE_FLASH_BASE + BOOT_SIZE;

/// App slot size: 227,328 B, `0x8000..0x3F800` (plan §1.2; supersedes R20's
/// 243,712). Upper bound for `image_size` and the SIZE command.
pub const APP_SLOT_SIZE: u32 = 0x0003_7800;

/// App slot erase-block count, derived — plan §1.2 fixes it at 111
/// (const-asserted below).
pub const APP_BLOCK_COUNT: u32 = APP_SLOT_SIZE / FLASH_BLOCK_SIZE;

/// Metadata block base: last 2 KB erase block, holding the 16-byte image
/// metadata record (SPEC §3.1), written last during an update.
pub const METADATA_ADDR: u32 = 0x0003_F800;

/// Metadata region is exactly one erase block.
pub const METADATA_SIZE: u32 = FLASH_BLOCK_SIZE;

/// SRAM base (SPEC §1.3). An initial SP equal to this would be a zero-size
/// stack and is rejected by validation.
pub const SRAM_BASE: u32 = 0x2000_0000;

/// SRAM size: 32 KB (SPEC §1.3).
pub const SRAM_SIZE: u32 = 0x0000_8000;

/// One past the last SRAM byte (`0x2000_8000`, exclusive) — legal as a
/// full-descending initial-SP top (SPEC §1.3).
pub const SRAM_END: u32 = SRAM_BASE + SRAM_SIZE;

// --- Compile-time partition consistency (plan §6 U0.4) ------------------

const _: () = assert!(
    BOOT_SIZE + APP_SLOT_SIZE + METADATA_SIZE == CODE_FLASH_SIZE,
    "bootloader + app slot + metadata block must tile the 256 KB code flash exactly"
);
const _: () = assert!(
    METADATA_ADDR == APP_BASE + APP_SLOT_SIZE,
    "metadata block must start immediately after the app slot"
);
const _: () = assert!(
    METADATA_ADDR + METADATA_SIZE == CODE_FLASH_SIZE,
    "metadata block must be the last erase block"
);
const _: () = assert!(
    APP_BASE == 0x8000,
    "plan \u{a7}1.2 locks APP_BASE at 0x8000"
);
const _: () = assert!(
    BOOT_SIZE.is_multiple_of(FLASH_BLOCK_SIZE),
    "bootloader region must be a whole number of erase blocks"
);
const _: () = assert!(
    APP_BASE.is_multiple_of(FLASH_BLOCK_SIZE),
    "APP_BASE must be erase-block aligned"
);
const _: () = assert!(
    METADATA_ADDR.is_multiple_of(FLASH_BLOCK_SIZE),
    "METADATA_ADDR must be erase-block aligned"
);
const _: () = assert!(
    APP_SLOT_SIZE.is_multiple_of(FLASH_BLOCK_SIZE),
    "app slot must be a whole number of erase blocks"
);
const _: () = assert!(
    APP_BLOCK_COUNT == 111,
    "plan \u{a7}1.2 locks the app slot at 111 blocks"
);

#[cfg(test)]
mod tests {
    use super::*;

    /// Restates the plan §1.2 literals so a constant edit shows up as a test
    /// diff too, not only as a const-assert compile error.
    #[test]
    fn plan_locked_values() {
        assert_eq!(APP_SLOT_SIZE, 227_328);
        assert_eq!(APP_BLOCK_COUNT, 111);
        assert_eq!(BOOT_SIZE, 32_768);
        assert_eq!(METADATA_ADDR, 0x0003_F800);
    }

    #[test]
    fn sram_bounds() {
        assert_eq!(SRAM_END, 0x2000_8000);
    }
}
