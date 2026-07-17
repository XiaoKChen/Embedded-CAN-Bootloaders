//! CRC-32 per SPEC §3.2 / R14: IEEE 802.3 / zlib-compatible — **reflected**,
//! polynomial `0xEDB88320`, init `0xFFFFFFFF`, final XOR `0xFFFFFFFF`.
//! Golden vector: `crc32(b"123456789") == 0xCBF43926`.
//!
//! Bitwise, table-less implementation — the same flash-size trade the C
//! bootloader made; throughput is irrelevant next to CAN at 250 kbit/s and
//! flash P/E times. The incremental [`Crc32`] state lets multi-hundred-KB
//! images stream through a small buffer (`image_meta` validation, VERIFY).

/// Reflected IEEE 802.3 polynomial (SPEC §3.2).
const POLYNOMIAL_REFLECTED: u32 = 0xEDB8_8320;

/// Initial shift-register value (SPEC §3.2).
const INIT: u32 = 0xFFFF_FFFF;

/// Final output XOR (SPEC §3.2).
const XOR_OUT: u32 = 0xFFFF_FFFF;

/// Incremental CRC-32 state. `new()` → any number of `update()`s →
/// `finalize()`. Chunking is transparent: any split of the input yields the
/// same CRC as one contiguous pass.
#[derive(Debug, Clone)]
pub struct Crc32 {
    state: u32,
}

impl Crc32 {
    /// Starts a fresh computation.
    #[must_use]
    pub const fn new() -> Self {
        Self { state: INIT }
    }

    /// Folds `data` into the running CRC.
    pub fn update(&mut self, data: &[u8]) {
        for &byte in data {
            self.state ^= u32::from(byte);
            let mut bit = 0;
            while bit < 8 {
                let lsb_set = self.state & 1 != 0;
                self.state >>= 1;
                if lsb_set {
                    self.state ^= POLYNOMIAL_REFLECTED;
                }
                bit += 1;
            }
        }
    }

    /// Consumes the state and returns the final CRC (applies the output XOR).
    /// The CRC of empty input is `0x0000_0000`.
    #[must_use]
    pub const fn finalize(self) -> u32 {
        self.state ^ XOR_OUT
    }
}

impl Default for Crc32 {
    fn default() -> Self {
        Self::new()
    }
}

/// One-shot CRC-32 of `data` with the SPEC §3.2 parameters.
#[must_use]
pub fn crc32(data: &[u8]) -> u32 {
    let mut crc = Crc32::new();
    crc.update(data);
    crc.finalize()
}

#[cfg(test)]
mod tests {
    use super::*;

    /// R14 golden vector.
    #[test]
    fn golden_vector() {
        assert_eq!(crc32(b"123456789"), 0xCBF4_3926);
    }

    #[test]
    fn empty_input() {
        assert_eq!(crc32(&[]), 0x0000_0000);
    }

    /// Splitting the input at every possible point (including empty updates)
    /// must equal the one-shot result.
    #[test]
    fn multi_chunk_equals_one_chunk() {
        let data = b"123456789";
        let expected = crc32(data);
        for split in 0..=data.len() {
            let mut crc = Crc32::new();
            crc.update(&data[..split]);
            crc.update(&data[split..]);
            assert_eq!(crc.finalize(), expected, "split at {split}");
        }
    }

    /// Byte-at-a-time streaming over a longer, non-trivial buffer.
    #[test]
    fn byte_at_a_time_streaming() {
        let data: Vec<u8> = (0..=255u8).cycle().take(4096).collect();
        let expected = crc32(&data);
        let mut crc = Crc32::new();
        for byte in &data {
            crc.update(core::slice::from_ref(byte));
        }
        assert_eq!(crc.finalize(), expected);
    }

    #[test]
    fn default_matches_new() {
        let mut a = Crc32::default();
        let mut b = Crc32::new();
        a.update(b"abc");
        b.update(b"abc");
        assert_eq!(a.finalize(), b.finalize());
    }
}
