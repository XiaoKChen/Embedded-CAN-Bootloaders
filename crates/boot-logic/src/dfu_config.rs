//! v1/v2 DFU config-block parsing (SPEC §6.2, R40) — flash-time USB names
//! plus per-device CAN-ID overrides.
//!
//! The block lives in data flash at [`DFU_CONFIG_ADDR`] and is a **frozen
//! external contract** shared with the ESP-Prog-2 provisioning flasher — the
//! layout never changes. The caller (the bootloader, behind the data-flash
//! read gate of SPEC §2.4/R34) hands in the raw bytes; parsing never touches
//! hardware and never panics on any byte pattern.
//!
//! Fallback ladder (R17/R40): unreadable/absent/corrupt block → compiled
//! defaults for everything; valid **v1** block → names from the block, both
//! CAN IDs default; valid **v2** block → names from the block plus each CAN
//! ID *independently* taken from the block when ≤ [`CAN_ID_MAX`], else that
//! ID's default (`0xFFFF`/out-of-range = "unset").

/// Data-flash address of the config block (SPEC §6.2 `DFU_CONFIG_ADDR`).
/// Read it only when the data-flash gate proved readability (R34).
pub const DFU_CONFIG_ADDR: u32 = 0x4010_1C00;

/// Config-block magic at offset 0, u32 LE (SPEC §6.2).
pub const DFU_CONFIG_MAGIC: u32 = 0xDF0C_0FDF;

/// v1 block length in bytes.
pub const V1_BLOCK_LEN: usize = 76;
/// v2 block length in bytes.
pub const V2_BLOCK_LEN: usize = 80;

/// Manufacturer/product name field width; also the INFO name field (SPEC
/// §4.7, §6.2).
pub const NAME_LEN: usize = 32;

/// Highest valid 11-bit CAN ID; v2 IDs above this are "unset" (SPEC §6.2).
pub const CAN_ID_MAX: u16 = 0x7FF;

/// Compiled-default host→boot command ID (SPEC §4.1 `CAN_ID_HOST_TO_BOOT`).
pub const DEFAULT_CMD_ID: u16 = 0x79E;
/// Compiled-default boot→host reply ID (SPEC §4.1 `CAN_ID_BOOT_TO_HOST`).
pub const DEFAULT_REPLY_ID: u16 = 0x79F;

const VERSION_OFFSET: usize = 0x04;
const MFR_LEN_OFFSET: usize = 0x05;
const PROD_LEN_OFFSET: usize = 0x06;
const MFR_OFFSET: usize = 0x08;
const PROD_OFFSET: usize = 0x28;
const V1_CRC_OFFSET: usize = 0x48;
const V2_CMD_ID_OFFSET: usize = 0x48;
const V2_REPLY_ID_OFFSET: usize = 0x4A;
const V2_CRC_OFFSET: usize = 0x4C;

/// Why a block was rejected, ordered to the check order (SPEC §6.2:
/// magic → version → lens → CRC). `Truncated` is an addition for this
/// slice-based API: the input ends before the field the current check needs
/// (the C code read fixed memory and could not observe truncation).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[non_exhaustive]
pub enum ParseError {
    /// Input shorter than the region the current check must read.
    Truncated,
    /// Magic ≠ `0xDF0C0FDF` (covers blank flash: all-`0xFF`/all-`0x00`).
    Magic,
    /// Version byte ∉ {1, 2}.
    Version,
    /// A name length byte > 32.
    Len,
    /// The version's CRC-32 does not match the covered bytes.
    Crc,
}

/// Effective device configuration after parsing + the fallback ladder.
/// Name bytes past each `*_len` are normalized to `0x00` (flash stores
/// `0xFF` there) so `product` feeds [`crate::protocol::build_info_reply`]
/// and USB string descriptors directly.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DfuConfig {
    /// USB manufacturer string bytes (ASCII), `0x00` past `manufacturer_len`.
    pub manufacturer: [u8; NAME_LEN],
    /// Valid bytes in `manufacturer`, 0..=32.
    pub manufacturer_len: u8,
    /// USB product / INFO name bytes (ASCII), `0x00` past `product_len`.
    pub product: [u8; NAME_LEN],
    /// Valid bytes in `product`, 0..=32.
    pub product_len: u8,
    /// Effective host→boot command ID (engine match + RX filter), ≤ `0x7FF`.
    pub cmd_id: u16,
    /// Effective boot→host reply ID (replies + INFO frames), ≤ `0x7FF`.
    pub reply_id: u16,
}

impl Default for DfuConfig {
    /// Compiled defaults: empty names, IDs `0x79E`/`0x79F` — the full
    /// fallback for an absent/invalid/unreadable block.
    fn default() -> Self {
        Self {
            manufacturer: [0x00; NAME_LEN],
            manufacturer_len: 0,
            product: [0x00; NAME_LEN],
            product_len: 0,
            cmd_id: DEFAULT_CMD_ID,
            reply_id: DEFAULT_REPLY_ID,
        }
    }
}

/// Parse a raw config block (SPEC §6.2 layout). `block` may be longer than
/// the block itself (e.g. a whole data-flash page); trailing bytes are
/// ignored. Checks, in order: magic, version, name lengths, version-specific
/// CRC-32. Never panics on any input.
pub fn parse(block: &[u8]) -> Result<DfuConfig, ParseError> {
    let header = block.get(..MFR_OFFSET).ok_or(ParseError::Truncated)?;
    let magic = u32::from_le_bytes([header[0], header[1], header[2], header[3]]);
    if magic != DFU_CONFIG_MAGIC {
        return Err(ParseError::Magic);
    }
    let crc_offset = match header[VERSION_OFFSET] {
        1 => V1_CRC_OFFSET,
        2 => V2_CRC_OFFSET,
        _ => return Err(ParseError::Version),
    };
    let mfr_len = header[MFR_LEN_OFFSET];
    let prod_len = header[PROD_LEN_OFFSET];
    if usize::from(mfr_len) > NAME_LEN || usize::from(prod_len) > NAME_LEN {
        return Err(ParseError::Len);
    }
    let covered = block.get(..crc_offset).ok_or(ParseError::Truncated)?;
    let stored_crc = le_u16_pair_u32(block, crc_offset).ok_or(ParseError::Truncated)?;
    if crate::crc32::crc32(covered) != stored_crc {
        return Err(ParseError::Crc);
    }

    let (cmd_id, reply_id) = if crc_offset == V2_CRC_OFFSET {
        // covered.len() == 0x4C, so both ID reads below are in range.
        let raw_cmd =
            u16::from_le_bytes([covered[V2_CMD_ID_OFFSET], covered[V2_CMD_ID_OFFSET + 1]]);
        let raw_reply =
            u16::from_le_bytes([covered[V2_REPLY_ID_OFFSET], covered[V2_REPLY_ID_OFFSET + 1]]);
        (
            id_or_default(raw_cmd, DEFAULT_CMD_ID),
            id_or_default(raw_reply, DEFAULT_REPLY_ID),
        )
    } else {
        // v1 carries no IDs (SPEC §6.2).
        (DEFAULT_CMD_ID, DEFAULT_REPLY_ID)
    };

    Ok(DfuConfig {
        manufacturer: name_field(covered, MFR_OFFSET, mfr_len),
        manufacturer_len: mfr_len,
        product: name_field(covered, PROD_OFFSET, prod_len),
        product_len: prod_len,
        cmd_id,
        reply_id,
    })
}

/// The fallback ladder in one call: `None` (data flash unproven/unread, R34)
/// or an invalid block yields [`DfuConfig::default`]; a valid block yields
/// [`parse`]'s result with its per-ID fallbacks already applied.
pub fn effective_config(block: Option<&[u8]>) -> DfuConfig {
    block
        .and_then(|bytes| parse(bytes).ok())
        .unwrap_or_default()
}

/// Per-ID fallback: in-range → use it; `0xFFFF`/out-of-range = unset →
/// that ID's compiled default (SPEC §6.2).
fn id_or_default(raw: u16, default: u16) -> u16 {
    if raw <= CAN_ID_MAX {
        raw
    } else {
        default
    }
}

/// Copy `len` name bytes from `block[offset..]`, zero-padding the rest
/// (flash stores `0xFF` past `len`; consumers want `0x00`, SPEC §4.7).
/// Caller guarantees `offset + len ≤ block.len()` (name fields precede the
/// CRC-covered region's end at 0x48).
fn name_field(block: &[u8], offset: usize, len: u8) -> [u8; NAME_LEN] {
    let mut name = [0x00; NAME_LEN];
    let len = usize::from(len); // ≤ NAME_LEN, checked in parse()
    name[..len].copy_from_slice(&block[offset..offset + len]);
    name
}

fn le_u16_pair_u32(block: &[u8], offset: usize) -> Option<u32> {
    let bytes = block.get(offset..offset + 4)?;
    Some(u32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]))
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Build a syntactically valid block; `ids = None` produces v1.
    fn build_block(mfr: &[u8], prod: &[u8], ids: Option<(u16, u16)>) -> Vec<u8> {
        let (version, total, crc_offset) = match ids {
            None => (1u8, V1_BLOCK_LEN, V1_CRC_OFFSET),
            Some(_) => (2u8, V2_BLOCK_LEN, V2_CRC_OFFSET),
        };
        let mut block = vec![0xFF_u8; total];
        block[..4].copy_from_slice(&DFU_CONFIG_MAGIC.to_le_bytes());
        block[VERSION_OFFSET] = version;
        block[MFR_LEN_OFFSET] = mfr.len() as u8;
        block[PROD_LEN_OFFSET] = prod.len() as u8;
        block[MFR_OFFSET..MFR_OFFSET + mfr.len()].copy_from_slice(mfr);
        block[PROD_OFFSET..PROD_OFFSET + prod.len()].copy_from_slice(prod);
        if let Some((cmd, reply)) = ids {
            block[V2_CMD_ID_OFFSET..V2_CMD_ID_OFFSET + 2].copy_from_slice(&cmd.to_le_bytes());
            block[V2_REPLY_ID_OFFSET..V2_REPLY_ID_OFFSET + 2].copy_from_slice(&reply.to_le_bytes());
        }
        let crc = crate::crc32::crc32(&block[..crc_offset]);
        block[crc_offset..crc_offset + 4].copy_from_slice(&crc.to_le_bytes());
        block
    }

    fn padded_name(name: &[u8]) -> [u8; NAME_LEN] {
        let mut out = [0x00; NAME_LEN];
        out[..name.len()].copy_from_slice(name);
        out
    }

    #[test]
    fn v1_valid_block_yields_names_and_default_ids() {
        let block = build_block(b"Normal Lab", b"Airform R4", None);
        let cfg = parse(&block).expect("valid v1 block");
        assert_eq!(cfg.manufacturer, padded_name(b"Normal Lab"));
        assert_eq!(cfg.manufacturer_len, 10);
        assert_eq!(cfg.product, padded_name(b"Airform R4"));
        assert_eq!(cfg.product_len, 10);
        assert_eq!(cfg.cmd_id, DEFAULT_CMD_ID);
        assert_eq!(cfg.reply_id, DEFAULT_REPLY_ID);
    }

    #[test]
    fn v2_valid_block_yields_names_and_override_ids() {
        let block = build_block(b"Mfr", b"Prod", Some((0x123, 0x124)));
        let cfg = parse(&block).expect("valid v2 block");
        assert_eq!(cfg.cmd_id, 0x123);
        assert_eq!(cfg.reply_id, 0x124);
        assert_eq!(cfg.product, padded_name(b"Prod"));
        assert_eq!(cfg.product_len, 4);
    }

    #[test]
    fn v2_id_range_edges() {
        // 0x000 and 0x7FF are both valid 11-bit IDs.
        let cfg = parse(&build_block(b"", b"", Some((0x000, 0x7FF)))).expect("valid");
        assert_eq!(cfg.cmd_id, 0x000);
        assert_eq!(cfg.reply_id, 0x7FF);
    }

    #[test]
    fn v2_out_of_range_ids_fall_back_independently() {
        // cmd unset (0xFFFF), reply valid.
        let cfg = parse(&build_block(b"", b"", Some((0xFFFF, 0x321)))).expect("valid");
        assert_eq!(cfg.cmd_id, DEFAULT_CMD_ID);
        assert_eq!(cfg.reply_id, 0x321);
        // cmd valid, reply just out of range (0x800).
        let cfg = parse(&build_block(b"", b"", Some((0x456, 0x800)))).expect("valid");
        assert_eq!(cfg.cmd_id, 0x456);
        assert_eq!(cfg.reply_id, DEFAULT_REPLY_ID);
        // Both out of range.
        let cfg = parse(&build_block(b"", b"", Some((0x800, 0xFFFF)))).expect("valid");
        assert_eq!(cfg.cmd_id, DEFAULT_CMD_ID);
        assert_eq!(cfg.reply_id, DEFAULT_REPLY_ID);
    }

    #[test]
    fn names_are_zero_padded_even_though_flash_stores_0xff() {
        // build_block leaves 0xFF past the name lengths, like real flash.
        let block = build_block(b"AB", b"C", Some((0x123, 0x124)));
        assert_eq!(block[MFR_OFFSET + 2], 0xFF);
        let cfg = parse(&block).expect("valid");
        assert_eq!(&cfg.manufacturer[2..], &[0x00; 30]);
        assert_eq!(&cfg.product[1..], &[0x00; 31]);
    }

    #[test]
    fn full_32_byte_names_are_accepted() {
        let name = [b'x'; NAME_LEN];
        let cfg = parse(&build_block(&name, &name, None)).expect("valid");
        assert_eq!(cfg.manufacturer, name);
        assert_eq!(cfg.manufacturer_len, 32);
    }

    #[test]
    fn wrong_magic_rejected_including_blank_flash() {
        let mut block = build_block(b"a", b"b", None);
        block[0] ^= 0x01;
        assert_eq!(parse(&block), Err(ParseError::Magic));
        assert_eq!(parse(&[0xFF; V2_BLOCK_LEN]), Err(ParseError::Magic));
        assert_eq!(parse(&[0x00; V2_BLOCK_LEN]), Err(ParseError::Magic));
    }

    #[test]
    fn bad_version_rejected() {
        for bad in [0u8, 3, 0x7F, 0xFF] {
            let mut block = build_block(b"a", b"b", None);
            block[VERSION_OFFSET] = bad;
            assert_eq!(parse(&block), Err(ParseError::Version));
        }
    }

    #[test]
    fn name_len_over_32_rejected() {
        for offset in [MFR_LEN_OFFSET, PROD_LEN_OFFSET] {
            let mut block = build_block(b"a", b"b", Some((1, 2)));
            block[offset] = 33;
            assert_eq!(parse(&block), Err(ParseError::Len));
        }
    }

    #[test]
    fn crc_mismatch_rejected_for_both_versions() {
        let mut v1 = build_block(b"a", b"b", None);
        v1[MFR_OFFSET] ^= 0x01; // corrupt a covered byte
        assert_eq!(parse(&v1), Err(ParseError::Crc));
        let mut v2 = build_block(b"a", b"b", Some((1, 2)));
        v2[V2_CRC_OFFSET] ^= 0x01; // corrupt the stored CRC itself
        assert_eq!(parse(&v2), Err(ParseError::Crc));
    }

    #[test]
    fn truncated_inputs_rejected_not_panicking() {
        let v1 = build_block(b"a", b"b", None);
        let v2 = build_block(b"a", b"b", Some((1, 2)));
        assert_eq!(parse(&[]), Err(ParseError::Truncated));
        assert_eq!(parse(&v1[..7]), Err(ParseError::Truncated));
        assert_eq!(parse(&v1[..V1_BLOCK_LEN - 1]), Err(ParseError::Truncated));
        assert_eq!(parse(&v2[..V2_BLOCK_LEN - 1]), Err(ParseError::Truncated));
        // A v1-sized slice of a v2 block cannot reach the v2 CRC.
        assert_eq!(parse(&v2[..V1_BLOCK_LEN]), Err(ParseError::Truncated));
    }

    #[test]
    fn check_order_matches_spec() {
        // Bad magic wins over bad version.
        let mut block = build_block(b"a", b"b", None);
        block[0] ^= 0x01;
        block[VERSION_OFFSET] = 9;
        assert_eq!(parse(&block), Err(ParseError::Magic));
        // Bad version wins over bad len.
        let mut block = build_block(b"a", b"b", None);
        block[VERSION_OFFSET] = 9;
        block[MFR_LEN_OFFSET] = 200;
        assert_eq!(parse(&block), Err(ParseError::Version));
        // Bad len wins over bad CRC (len edits also break the CRC).
        let mut block = build_block(b"a", b"b", None);
        block[MFR_LEN_OFFSET] = 200;
        assert_eq!(parse(&block), Err(ParseError::Len));
    }

    #[test]
    fn trailing_bytes_beyond_the_block_are_ignored() {
        // e.g. the caller hands the whole 1 KB data-flash region.
        let mut region = vec![0xFF_u8; 1024];
        let block = build_block(b"Mfr", b"Prod", Some((0x100, 0x101)));
        region[..block.len()].copy_from_slice(&block);
        let cfg = parse(&region).expect("valid block with trailing flash");
        assert_eq!(cfg.cmd_id, 0x100);
        assert_eq!(cfg.reply_id, 0x101);
    }

    #[test]
    fn effective_config_fallback_ladder() {
        // Unreadable data flash (R34).
        assert_eq!(effective_config(None), DfuConfig::default());
        // Corrupt block.
        assert_eq!(effective_config(Some(&[0xFF; 80])), DfuConfig::default());
        // Valid v2 block.
        let block = build_block(b"M", b"P", Some((0x111, 0x222)));
        let cfg = effective_config(Some(&block));
        assert_eq!(cfg.cmd_id, 0x111);
        assert_eq!(cfg.reply_id, 0x222);
        // Defaults are the SPEC §4.1 IDs.
        assert_eq!(DfuConfig::default().cmd_id, 0x79E);
        assert_eq!(DfuConfig::default().reply_id, 0x79F);
        assert_eq!(DfuConfig::default().product_len, 0);
    }

    /// Fault injection (rules/testing.md): no byte pattern may panic.
    #[test]
    fn adversarial_buffers_never_panic() {
        let mut cases: Vec<Vec<u8>> = vec![
            vec![],
            vec![0xDF],
            DFU_CONFIG_MAGIC.to_le_bytes().to_vec(),
            vec![0xFF; 7],
            vec![0x00; 8],
            vec![0xAA; 75],
            vec![0x55; 79],
            vec![0xFF; 4096],
        ];
        // Valid header claiming each version but truncated at every length.
        let full = build_block(&[b'n'; 32], &[b'p'; 32], Some((0x7FF, 0x7FF)));
        for len in 0..full.len() {
            cases.push(full[..len].to_vec());
        }
        // Every single-byte corruption of a valid block.
        for i in 0..full.len() {
            let mut corrupt = full.clone();
            corrupt[i] ^= 0xFF;
            cases.push(corrupt);
        }
        for case in &cases {
            let _ = parse(case); // must not panic
            let _ = effective_config(Some(case));
        }
    }
}
