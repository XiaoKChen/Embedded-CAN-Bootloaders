//! Image metadata codec + validation (SPEC §3.1/§3.3, R10/R11/R15).
//!
//! The 16-byte little-endian metadata record lives at
//! [`memory_map::METADATA_ADDR`] and is written **last** during an update —
//! the commit point. Validation runs the SPEC §3.3 check order (fail closed,
//! first failure wins) over the [`ImageRead`] seam so this crate never
//! touches hardware; the bootloader implements the trait in `flash_read.rs`
//! with volatile raw-pointer copies (references to address 0 are UB —
//! SPEC §9.1.7). Size bounds use the plan §1.2 slot (227,328 B), superseding
//! SPEC §1.1 / R20 values.
//!
//! Error enums derive plain `Debug` only; `defmt::Format` is attached at the
//! firmware boundary later (deliberate deviation recorded in the plan).

use crate::crc32::{crc32, Crc32};
use crate::memory_map::{APP_BASE, APP_SLOT_SIZE, METADATA_ADDR, SRAM_BASE, SRAM_END};

/// Metadata record magic (SPEC §3.1, `IMAGE_META_MAGIC`).
pub const IMAGE_META_MAGIC: u32 = 0xB007_D00D;

/// Metadata record length in bytes (SPEC §3.1).
pub const METADATA_LEN: usize = 16;

/// The header CRC covers the first 12 bytes of the record as stored (SPEC §3.1).
const HEADER_CRC_COVERAGE: usize = 12;

/// Minimum valid `image_size`: the SP + entry words (SPEC §3.3 step 3).
pub const MIN_IMAGE_SIZE: u32 = 8;

/// Maximum valid `image_size`: the full app slot (plan §1.2; supersedes R20).
pub const MAX_IMAGE_SIZE: u32 = APP_SLOT_SIZE;

/// Thumb bit of a Cortex-M vector-table entry address.
const THUMB_BIT: u32 = 1;

/// Stock fallback check (SPEC §2 step 4, R11): high byte of the initial-SP
/// word must be `0x20` — i.e. the SP points somewhere into the SRAM region.
const STOCK_SP_WORD_MASK: u32 = 0xFF00_0000;
const STOCK_SP_WORD_EXPECTED: u32 = 0x2000_0000;

/// Stack-buffer size for streaming the payload CRC (SPEC §3.3 step 6);
/// sized small because it lives on the boot-decision stack frame.
const PAYLOAD_CRC_CHUNK: u32 = 64;

/// Incremental code-flash read seam (docs/plan.md §3). `offset` is the byte
/// offset from the code-flash base (address 0), so callers pass the
/// `memory_map` constants (`APP_BASE`, `METADATA_ADDR`) directly.
pub trait ImageRead {
    /// Fills `buf` entirely from flash starting at `offset`, or fails —
    /// partial reads are not part of the contract.
    fn read(&self, offset: u32, buf: &mut [u8]) -> Result<(), ReadError>;
}

/// Failure of an [`ImageRead`] read.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[non_exhaustive]
pub enum ReadError {
    /// The requested range extends past the readable flash region.
    OutOfBounds,
}

/// Image validation failure (SPEC §3.3) — one variant per documented error
/// code, in check order. First failure wins; validation is fail-closed.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[non_exhaustive]
pub enum ImageError {
    /// Step 1: metadata magic != [`IMAGE_META_MAGIC`] (absent/erased metadata).
    Magic,
    /// Step 2: CRC over the first 12 stored bytes != `header_crc32`.
    HeaderCrc,
    /// Step 3: `image_size` outside `[MIN_IMAGE_SIZE, MAX_IMAGE_SIZE]`.
    Size,
    /// Step 4: initial SP word not in `(SRAM_BASE, SRAM_END]`.
    StackPointer,
    /// Step 5: reset vector missing the Thumb bit, or its address (Thumb bit
    /// cleared) outside `[APP_BASE, APP_BASE + APP_SLOT_SIZE)`.
    Entry,
    /// Step 6: CRC over `[APP_BASE, APP_BASE + image_size)` != `image_crc32`.
    PayloadCrc,
    /// The reader failed; validation could not complete (fail closed).
    Read(ReadError),
}

impl From<ReadError> for ImageError {
    fn from(err: ReadError) -> Self {
        Self::Read(err)
    }
}

/// The 16-byte metadata record (SPEC §3.1). `parse` is a pure decode —
/// validation (magic, CRCs, ranges) happens separately in [`validate_image`].
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct MetadataRecord {
    /// Offset 0x0: [`IMAGE_META_MAGIC`] on a committed image.
    pub magic: u32,
    /// Offset 0x4: bytes covered by `image_crc32`, counted from `APP_BASE`.
    pub image_size: u32,
    /// Offset 0x8: CRC-32 over flash `[APP_BASE, APP_BASE + image_size)`.
    pub image_crc32: u32,
    /// Offset 0xC: CRC-32 over the first 12 bytes of this record as stored.
    pub header_crc32: u32,
}

impl MetadataRecord {
    /// Decodes the record from its stored little-endian form. Never fails:
    /// garbage in, garbage fields out — [`validate_image`] judges them.
    #[must_use]
    pub fn parse(bytes: &[u8; METADATA_LEN]) -> Self {
        Self {
            magic: read_u32_le(bytes, 0x0),
            image_size: read_u32_le(bytes, 0x4),
            image_crc32: read_u32_le(bytes, 0x8),
            header_crc32: read_u32_le(bytes, 0xC),
        }
    }

    /// Encodes the record to its stored little-endian form (the exact 16
    /// bytes programmed at [`METADATA_ADDR`]).
    #[must_use]
    pub fn to_bytes(&self) -> [u8; METADATA_LEN] {
        let mut out = [0u8; METADATA_LEN];
        out[0x0..0x4].copy_from_slice(&self.magic.to_le_bytes());
        out[0x4..0x8].copy_from_slice(&self.image_size.to_le_bytes());
        out[0x8..0xC].copy_from_slice(&self.image_crc32.to_le_bytes());
        out[0xC..0x10].copy_from_slice(&self.header_crc32.to_le_bytes());
        out
    }

    /// Builds the commit record for a verified image: magic fixed,
    /// `header_crc32` computed over the first 12 bytes as stored (R15).
    #[must_use]
    pub fn for_image(image_size: u32, image_crc32: u32) -> Self {
        let mut record = Self {
            magic: IMAGE_META_MAGIC,
            image_size,
            image_crc32,
            header_crc32: 0,
        };
        record.header_crc32 = crc32(&record.to_bytes()[..HEADER_CRC_COVERAGE]);
        record
    }
}

/// Full image validation in SPEC §3.3 order — fail closed, first failure
/// wins. Returns the parsed metadata on success (callers reuse
/// `image_size`/`image_crc32` for INFO replies etc.).
pub fn validate_image<R: ImageRead>(flash: &R) -> Result<MetadataRecord, ImageError> {
    let mut meta_bytes = [0u8; METADATA_LEN];
    flash.read(METADATA_ADDR, &mut meta_bytes)?;
    let meta = MetadataRecord::parse(&meta_bytes);

    // 1. Magic.
    if meta.magic != IMAGE_META_MAGIC {
        return Err(ImageError::Magic);
    }
    // 2. Header CRC over the record's first 12 bytes as stored.
    if crc32(&meta_bytes[..HEADER_CRC_COVERAGE]) != meta.header_crc32 {
        return Err(ImageError::HeaderCrc);
    }
    // 3. Size bounds.
    if meta.image_size < MIN_IMAGE_SIZE || meta.image_size > MAX_IMAGE_SIZE {
        return Err(ImageError::Size);
    }
    // 4/5. Initial SP and reset vector from the app vector table.
    let mut vectors = [0u8; 8];
    flash.read(APP_BASE, &mut vectors)?;
    let initial_sp = read_u32_le(&vectors, 0);
    if initial_sp <= SRAM_BASE || initial_sp > SRAM_END {
        return Err(ImageError::StackPointer);
    }
    let reset_vector = read_u32_le(&vectors, 4);
    if reset_vector & THUMB_BIT == 0 {
        return Err(ImageError::Entry);
    }
    let entry_addr = reset_vector & !THUMB_BIT;
    if !(APP_BASE..APP_BASE + APP_SLOT_SIZE).contains(&entry_addr) {
        return Err(ImageError::Entry);
    }
    // 6. Payload CRC — the expensive check runs last, streamed through a
    // small stack buffer so no full-image buffer ever exists.
    let mut buf = [0u8; PAYLOAD_CRC_CHUNK as usize];
    let mut crc = Crc32::new();
    let mut offset: u32 = 0;
    while offset < meta.image_size {
        let take = (meta.image_size - offset).min(PAYLOAD_CRC_CHUNK);
        // take <= 64: u32 -> usize is a lossless widening here.
        let chunk = &mut buf[..take as usize];
        flash.read(APP_BASE + offset, chunk)?;
        crc.update(chunk);
        offset += take;
    }
    if crc.finalize() != meta.image_crc32 {
        return Err(ImageError::PayloadCrc);
    }
    Ok(meta)
}

/// Stock SP-word plausibility check (SPEC §2 step 4, R11): the word at
/// `APP_BASE` must have high byte `0x20` (points into SRAM). Deliberately
/// looser than step 4 of [`validate_image`] — byte-identical to the stock
/// bootloader's check so SWD-flashed images without metadata still boot.
pub fn stock_sp_word_plausible<R: ImageRead>(flash: &R) -> Result<bool, ReadError> {
    let mut sp_word = [0u8; 4];
    flash.read(APP_BASE, &mut sp_word)?;
    Ok(u32::from_le_bytes(sp_word) & STOCK_SP_WORD_MASK == STOCK_SP_WORD_EXPECTED)
}

/// Boot-decision app check (SPEC §2 step 4, R10 + R11): full validation,
/// falling back to [`stock_sp_word_plausible`] **only** when the metadata is
/// absent/corrupt (magic or header-CRC failure). Every other validation
/// failure is fail-closed; read failures propagate.
pub fn app_bootable<R: ImageRead>(flash: &R) -> Result<bool, ReadError> {
    match validate_image(flash) {
        Ok(_) => Ok(true),
        Err(ImageError::Magic | ImageError::HeaderCrc) => stock_sp_word_plausible(flash),
        Err(ImageError::Read(err)) => Err(err),
        Err(_) => Ok(false),
    }
}

/// Explicit little-endian u32 load — no aliased/misaligned pointer loads
/// (SPEC §3.3).
fn read_u32_le(bytes: &[u8], offset: usize) -> u32 {
    u32::from_le_bytes([
        bytes[offset],
        bytes[offset + 1],
        bytes[offset + 2],
        bytes[offset + 3],
    ])
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::memory_map::CODE_FLASH_SIZE;

    /// Full 256 KB fake code flash backed by a `Vec`.
    struct FakeFlash {
        data: Vec<u8>,
    }

    impl ImageRead for FakeFlash {
        fn read(&self, offset: u32, buf: &mut [u8]) -> Result<(), ReadError> {
            let start = offset as usize;
            let end = start.checked_add(buf.len()).ok_or(ReadError::OutOfBounds)?;
            if end > self.data.len() {
                return Err(ReadError::OutOfBounds);
            }
            buf.copy_from_slice(&self.data[start..end]);
            Ok(())
        }
    }

    /// Wrapper that fails every read at or past `fail_from` — models a
    /// faulty/truncated flash mid-payload.
    struct FaultyFrom<'a> {
        inner: &'a FakeFlash,
        fail_from: u32,
    }

    impl ImageRead for FaultyFrom<'_> {
        fn read(&self, offset: u32, buf: &mut [u8]) -> Result<(), ReadError> {
            if offset >= self.fail_from {
                return Err(ReadError::OutOfBounds);
            }
            self.inner.read(offset, buf)
        }
    }

    const VALID_SP: u32 = SRAM_END;
    const VALID_ENTRY: u32 = (APP_BASE + 8) | THUMB_BIT;

    fn erased_flash() -> FakeFlash {
        FakeFlash {
            data: vec![0xFF; CODE_FLASH_SIZE as usize],
        }
    }

    /// Payload with a plausible vector table and a deterministic body.
    fn valid_payload(size: usize) -> Vec<u8> {
        assert!(size >= 8);
        let mut payload: Vec<u8> = (0..size).map(|i| i as u8).collect();
        payload[0..4].copy_from_slice(&VALID_SP.to_le_bytes());
        payload[4..8].copy_from_slice(&VALID_ENTRY.to_le_bytes());
        payload
    }

    fn install_app(flash: &mut FakeFlash, payload: &[u8]) {
        let base = APP_BASE as usize;
        flash.data[base..base + payload.len()].copy_from_slice(payload);
    }

    fn install_meta(flash: &mut FakeFlash, record: &MetadataRecord) {
        let base = METADATA_ADDR as usize;
        flash.data[base..base + METADATA_LEN].copy_from_slice(&record.to_bytes());
    }

    fn flash_with_valid_image(size: usize) -> FakeFlash {
        let mut flash = erased_flash();
        let payload = valid_payload(size);
        install_app(&mut flash, &payload);
        install_meta(
            &mut flash,
            &MetadataRecord::for_image(size as u32, crc32(&payload)),
        );
        flash
    }

    // --- Codec ----------------------------------------------------------

    #[test]
    fn parse_explicit_little_endian_bytes() {
        let bytes: [u8; METADATA_LEN] = [
            0x0D, 0xD0, 0x07, 0xB0, // magic 0xB007D00D LE
            0x78, 0x56, 0x34, 0x12, // image_size 0x12345678
            0xEF, 0xBE, 0xAD, 0xDE, // image_crc32 0xDEADBEEF
            0x0D, 0xF0, 0xFE, 0xCA, // header_crc32 0xCAFEF00D
        ];
        let record = MetadataRecord::parse(&bytes);
        assert_eq!(record.magic, IMAGE_META_MAGIC);
        assert_eq!(record.image_size, 0x1234_5678);
        assert_eq!(record.image_crc32, 0xDEAD_BEEF);
        assert_eq!(record.header_crc32, 0xCAFE_F00D);
    }

    #[test]
    fn encode_parse_roundtrip() {
        let record = MetadataRecord::for_image(227_328, 0x0BAD_F00D);
        assert_eq!(MetadataRecord::parse(&record.to_bytes()), record);
    }

    #[test]
    fn for_image_header_crc_covers_first_12_bytes() {
        let record = MetadataRecord::for_image(1024, 0x1234_5678);
        let bytes = record.to_bytes();
        assert_eq!(record.magic, IMAGE_META_MAGIC);
        assert_eq!(crc32(&bytes[..HEADER_CRC_COVERAGE]), record.header_crc32);
    }

    // --- Validation: success + every error code --------------------------

    #[test]
    fn valid_image_passes_and_returns_record() {
        let flash = flash_with_valid_image(1024);
        let meta = validate_image(&flash).unwrap();
        assert_eq!(meta.image_size, 1024);
    }

    #[test]
    fn erased_metadata_fails_magic() {
        // Erased flash: metadata reads all-0xFF.
        assert_eq!(validate_image(&erased_flash()), Err(ImageError::Magic));
    }

    #[test]
    fn corrupt_header_crc_fails_header_crc() {
        let mut flash = flash_with_valid_image(1024);
        // Flip a bit inside the CRC-covered 12 bytes (image_size field).
        flash.data[METADATA_ADDR as usize + 0x4] ^= 0x01;
        assert_eq!(validate_image(&flash), Err(ImageError::HeaderCrc));
    }

    #[test]
    fn size_zero_fails_size() {
        let mut flash = flash_with_valid_image(1024);
        install_meta(&mut flash, &MetadataRecord::for_image(0, 0));
        assert_eq!(validate_image(&flash), Err(ImageError::Size));
    }

    #[test]
    fn size_one_fails_size() {
        let mut flash = flash_with_valid_image(1024);
        install_meta(&mut flash, &MetadataRecord::for_image(1, 0));
        assert_eq!(validate_image(&flash), Err(ImageError::Size));
    }

    #[test]
    fn size_seven_fails_size_and_eight_passes() {
        let mut flash = flash_with_valid_image(8);
        assert!(
            validate_image(&flash).is_ok(),
            "MIN_IMAGE_SIZE (8) must validate"
        );
        let payload = valid_payload(8);
        install_meta(
            &mut flash,
            &MetadataRecord::for_image(7, crc32(&payload[..7])),
        );
        assert_eq!(validate_image(&flash), Err(ImageError::Size));
    }

    #[test]
    fn size_full_slot_passes() {
        let flash = flash_with_valid_image(APP_SLOT_SIZE as usize);
        assert!(validate_image(&flash).is_ok());
    }

    #[test]
    fn size_slot_plus_one_fails_size() {
        let mut flash = flash_with_valid_image(1024);
        install_meta(&mut flash, &MetadataRecord::for_image(APP_SLOT_SIZE + 1, 0));
        assert_eq!(validate_image(&flash), Err(ImageError::Size));
    }

    fn flash_with_vector_words(sp: u32, entry: u32) -> FakeFlash {
        let mut flash = erased_flash();
        let mut payload = valid_payload(1024);
        payload[0..4].copy_from_slice(&sp.to_le_bytes());
        payload[4..8].copy_from_slice(&entry.to_le_bytes());
        install_app(&mut flash, &payload);
        install_meta(
            &mut flash,
            &MetadataRecord::for_image(1024, crc32(&payload)),
        );
        flash
    }

    #[test]
    fn sp_boundaries() {
        // SRAM_BASE itself = zero-size stack: illegal.
        assert_eq!(
            validate_image(&flash_with_vector_words(SRAM_BASE, VALID_ENTRY)),
            Err(ImageError::StackPointer)
        );
        // Smallest legal SP.
        assert!(validate_image(&flash_with_vector_words(SRAM_BASE + 4, VALID_ENTRY)).is_ok());
        // SRAM_END is a legal full-descending stack top.
        assert!(validate_image(&flash_with_vector_words(SRAM_END, VALID_ENTRY)).is_ok());
        // One word past the top: illegal.
        assert_eq!(
            validate_image(&flash_with_vector_words(SRAM_END + 4, VALID_ENTRY)),
            Err(ImageError::StackPointer)
        );
    }

    #[test]
    fn entry_missing_thumb_bit_fails_entry() {
        assert_eq!(
            validate_image(&flash_with_vector_words(VALID_SP, APP_BASE + 8)),
            Err(ImageError::Entry)
        );
    }

    #[test]
    fn entry_range_boundaries() {
        // Just below the slot.
        assert_eq!(
            validate_image(&flash_with_vector_words(
                VALID_SP,
                (APP_BASE - 2) | THUMB_BIT
            )),
            Err(ImageError::Entry)
        );
        // First legal address (APP_BASE itself, Thumb).
        assert!(validate_image(&flash_with_vector_words(VALID_SP, APP_BASE | THUMB_BIT)).is_ok());
        // Last legal address inside the slot.
        let last_inside = (APP_BASE + APP_SLOT_SIZE - 2) | THUMB_BIT;
        assert!(validate_image(&flash_with_vector_words(VALID_SP, last_inside)).is_ok());
        // First address past the slot (the metadata block).
        assert_eq!(
            validate_image(&flash_with_vector_words(
                VALID_SP,
                METADATA_ADDR | THUMB_BIT
            )),
            Err(ImageError::Entry)
        );
    }

    #[test]
    fn corrupt_payload_fails_payload_crc() {
        let mut flash = flash_with_valid_image(1024);
        // Corrupt one payload byte past the vector table.
        flash.data[APP_BASE as usize + 512] ^= 0xFF;
        assert_eq!(validate_image(&flash), Err(ImageError::PayloadCrc));
    }

    // --- Order of checks: first failure wins ------------------------------

    #[test]
    fn bad_magic_reported_before_bad_header_crc() {
        let mut flash = flash_with_valid_image(1024);
        let meta_base = METADATA_ADDR as usize;
        flash.data[meta_base] ^= 0xFF; // breaks magic AND the header CRC
        assert_eq!(validate_image(&flash), Err(ImageError::Magic));
    }

    #[test]
    fn bad_header_crc_reported_before_bad_size() {
        let mut flash = flash_with_valid_image(1024);
        // Oversize image_size with a stale header CRC: both step 2 and 3 fail.
        let mut record = MetadataRecord::for_image(1024, 0);
        record.image_size = APP_SLOT_SIZE + 1;
        install_meta(&mut flash, &record);
        assert_eq!(validate_image(&flash), Err(ImageError::HeaderCrc));
    }

    #[test]
    fn bad_size_reported_before_bad_sp() {
        let mut flash = erased_flash(); // app slot erased: SP word 0xFFFFFFFF
        install_meta(&mut flash, &MetadataRecord::for_image(0, 0));
        assert_eq!(validate_image(&flash), Err(ImageError::Size));
    }

    #[test]
    fn bad_sp_reported_before_bad_entry() {
        // Both vector words garbage: step 4 fires first.
        assert_eq!(
            validate_image(&flash_with_vector_words(0xFFFF_FFFF, 0xFFFF_FFFE)),
            Err(ImageError::StackPointer)
        );
    }

    #[test]
    fn bad_entry_reported_before_bad_payload_crc() {
        let mut flash = flash_with_vector_words(VALID_SP, 0xFFFF_FFFF); // entry out of slot
        flash.data[APP_BASE as usize + 512] ^= 0xFF; // payload CRC also broken
        assert_eq!(validate_image(&flash), Err(ImageError::Entry));
    }

    // --- Faulty readers propagate, never panic ----------------------------

    #[test]
    fn truncated_flash_propagates_read_error_from_metadata() {
        let flash = FakeFlash {
            data: vec![0xFF; METADATA_ADDR as usize],
        };
        assert_eq!(
            validate_image(&flash),
            Err(ImageError::Read(ReadError::OutOfBounds))
        );
    }

    #[test]
    fn read_failure_mid_payload_propagates() {
        let flash = flash_with_valid_image(4096);
        // Metadata + vector-table reads succeed; payload streaming hits the fault.
        let faulty = FaultyFrom {
            inner: &flash,
            fail_from: APP_BASE + 1024,
        };
        assert_eq!(
            validate_image(&faulty),
            Err(ImageError::Read(ReadError::OutOfBounds))
        );
    }

    // --- Fallback SP-word check (R11) --------------------------------------

    #[test]
    fn stock_sp_word_accepts_sram_pointer() {
        let flash = flash_with_vector_words(0x2000_1234, VALID_ENTRY);
        assert_eq!(stock_sp_word_plausible(&flash), Ok(true));
    }

    #[test]
    fn stock_sp_word_rejects_erased_and_non_sram() {
        assert_eq!(stock_sp_word_plausible(&erased_flash()), Ok(false));
        let flash = flash_with_vector_words(0x1FFF_FFFC, VALID_ENTRY);
        assert_eq!(stock_sp_word_plausible(&flash), Ok(false));
    }

    #[test]
    fn app_bootable_valid_image() {
        assert_eq!(app_bootable(&flash_with_valid_image(1024)), Ok(true));
    }

    #[test]
    fn app_bootable_falls_back_on_missing_metadata() {
        // SWD-flashed app, no metadata: plausible SP word → bootable.
        let mut flash = erased_flash();
        install_app(&mut flash, &valid_payload(1024));
        assert_eq!(app_bootable(&flash), Ok(true));
    }

    #[test]
    fn app_bootable_falls_back_on_corrupt_header_crc() {
        let mut flash = flash_with_valid_image(1024);
        flash.data[METADATA_ADDR as usize + 0xC] ^= 0xFF; // break header_crc32
        assert_eq!(app_bootable(&flash), Ok(true));
    }

    #[test]
    fn app_bootable_empty_flash_is_not_bootable() {
        assert_eq!(app_bootable(&erased_flash()), Ok(false));
    }

    #[test]
    fn app_bootable_no_fallback_for_later_failures() {
        // Valid metadata header but oversize image: fail closed even though
        // the SP word is plausible (R11 restricts fallback to magic/header).
        let mut flash = flash_with_valid_image(1024);
        install_meta(&mut flash, &MetadataRecord::for_image(APP_SLOT_SIZE + 1, 0));
        assert_eq!(app_bootable(&flash), Ok(false));
        // Same for a payload-CRC failure.
        let mut flash = flash_with_valid_image(1024);
        flash.data[APP_BASE as usize + 512] ^= 0xFF;
        assert_eq!(app_bootable(&flash), Ok(false));
    }

    #[test]
    fn app_bootable_propagates_read_errors() {
        let flash = FakeFlash {
            data: vec![0xFF; METADATA_ADDR as usize],
        };
        assert_eq!(app_bootable(&flash), Err(ReadError::OutOfBounds));
    }
}
