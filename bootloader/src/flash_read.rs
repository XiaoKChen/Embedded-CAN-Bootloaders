//! `ImageRead` over the memory-mapped code flash, via volatile raw-pointer reads only.
//!
//! Implements `boot_logic::image_meta::ImageRead` for the 256 KB code flash (U1.1,
//! docs/plan.md §3).
//!
//! Not here:    flash programming or erase (`drivers/flash_lp.rs`).
//! Invariants:  no Rust reference into flash is ever formed: code flash is mapped at address 0
//!              (SPEC §1.1), and a reference to address 0 is undefined behavior no matter how it
//!              is produced (SPEC §9.1.7). Every access computes the byte address as an integer,
//!              casts it to a raw pointer, and `read_volatile`s it.
//! Concurrency: stateless zero-size type; reads are side-effect free.
//! Errors:      [`ReadError::OutOfBounds`] when a range leaves the code flash.
//! Depends on:  `boot_logic` (`image_meta`, `memory_map`).
//! See also:    `main.rs` (boot decision).

use boot_logic::image_meta::{ImageRead, ReadError};
use boot_logic::memory_map::CODE_FLASH_SIZE;

/// Zero-size reader for the 256 KB code flash at address 0. `offset` in
/// [`ImageRead::read`] is the byte offset from the code-flash base, i.e. the
/// absolute address — callers pass `boot_logic::memory_map` constants
/// (`APP_BASE`, `METADATA_ADDR`) directly.
pub struct CodeFlashReader;

impl ImageRead for CodeFlashReader {
    /// Fills `buf` from flash `[offset, offset + buf.len())`, or fails with
    /// [`ReadError::OutOfBounds`] if the range leaves the 256 KB code flash.
    fn read(&self, offset: u32, buf: &mut [u8]) -> Result<(), ReadError> {
        let len = u32::try_from(buf.len()).map_err(|_| ReadError::OutOfBounds)?;
        let end = offset.checked_add(len).ok_or(ReadError::OutOfBounds)?;
        if end > CODE_FLASH_SIZE {
            return Err(ReadError::OutOfBounds);
        }
        for (i, byte) in buf.iter_mut().enumerate() {
            let addr = offset as usize + i;
            // SAFETY: addr < CODE_FLASH_SIZE (checked above) lies inside the
            // memory-mapped, always-readable code flash. The volatile read
            // through a freshly cast integer address keeps the compiler from
            // applying null/dereferenceable reasoning to the flash window
            // (SPEC §9.1.7) and prevents elision/reordering of the load.
            *byte = unsafe { (addr as *const u8).read_volatile() };
        }
        Ok(())
    }
}
