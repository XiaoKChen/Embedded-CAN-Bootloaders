//! PRC1-unlocked VBTBKR (backup register) read/clear/write (SPEC §1.4).
//!
//! `VBTBKR` is a byte array in the VBATT backup domain (R01UM0007, Battery
//! Backup Function chapter): its contents survive every reset — including
//! `NVIC_SystemReset()` and pin reset — and clear only on a VBATT-domain
//! power-on reset. The bootloader and app use it for boot-mode handoff
//! words. Writes require the PRCR.PRC1 unlock (key 0xA5); reads do not.
//!
//! Word layout (SPEC §1.4) — offsets are *byte* offsets into VBTBKR, each
//! word is 4 bytes little-endian:

use crate::clocks::{PRCR_KEY, PRCR_PRC1_LPM_VBT};
use ra4m1::SYSTEM;

/// Byte offset of the stock double-tap / DFU-detach word
/// (magic `0x07738135`, SPEC §1.4).
pub const DOUBLE_TAP_WORD_OFFSET: usize = 0;
/// Byte offset of the app-requested CAN-update word
/// (magic `0xB007CAFE`, SPEC §1.4).
pub const CAN_UPDATE_WORD_OFFSET: usize = 4;

/// Read the 4-byte little-endian word at `VBTBKR[byte_offset..+4]`.
///
/// `byte_offset` must be one of the named `*_WORD_OFFSET` constants: the
/// PAC exposes 512 byte slots but the RA4M1's real VBTBKR count is smaller
/// (unverified, SPEC §9.4) — this project only ever touches bytes 0..12.
pub fn read_word(system: &SYSTEM, byte_offset: usize) -> u32 {
    let mut bytes = [0u8; 4];
    for (i, byte) in bytes.iter_mut().enumerate() {
        *byte = system.vbtbkr[byte_offset + i].read().vbtbkr().bits();
    }
    u32::from_le_bytes(bytes)
}

/// Write `value` little-endian to `VBTBKR[byte_offset..+4]` under the PRC1
/// unlock. Same `byte_offset` contract as [`read_word`].
pub fn write_word(system: &SYSTEM, byte_offset: usize, value: u32) {
    // SAFETY: whole-register PRCR write of the documented key + PRC1 value
    // (R01UM0007, Register Write Protection) — unlocks the battery-backup
    // registers for the byte writes below; re-locked before returning.
    system
        .prcr
        .write(|w| unsafe { w.bits(PRCR_KEY | PRCR_PRC1_LPM_VBT) });
    for (i, byte) in value.to_le_bytes().iter().enumerate() {
        // SAFETY: VBTBKR is a full 8-bit data field — every u8 value is
        // valid (R01UM0007, Battery Backup Function, VBTBKR description).
        system.vbtbkr[byte_offset + i].write(|w| unsafe { w.vbtbkr().bits(*byte) });
    }
    // SAFETY: whole-register PRCR write of the documented key (re-lock).
    system.prcr.write(|w| unsafe { w.bits(PRCR_KEY) });
}

/// Clear the word at `byte_offset` to 0 (the "handoff consumed" state the
/// boot decision expects, SPEC §2).
pub fn clear_word(system: &SYSTEM, byte_offset: usize) {
    write_word(system, byte_offset, 0);
}
