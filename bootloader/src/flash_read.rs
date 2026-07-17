//! `boot_logic::image_meta::ImageRead` over the memory-mapped code flash —
//! volatile raw-pointer reads only (U1.1, docs/plan.md §3).
//!
//! Code flash is mapped at address 0 (SPEC §1.1), and a Rust **reference**
//! to address 0 is undefined behavior no matter how it is produced
//! (SPEC §9.1.7). Every access here therefore computes the byte address as
//! an integer, casts it to a raw pointer, and `read_volatile`s it — no
//! references into flash, no pointer arithmetic on a possibly-null base.

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
