//! OFS0/OFS1/Security-MPU option-byte words emitted into the `.ofs` linker
//! section at flash `0x400..0x440` (SPEC §1.5, R4).
//!
//! **Brick risk — review like unsafe code.** These 64 bytes live inside the
//! bootloader image and are latched by hardware at reset: a wrong OFS0
//! auto-starts the IWDT against a bootloader that never feeds it. The
//! values below must stay byte-identical to the stock Arduino bootloader
//! dump (`main:docs/build.md`, "Pre-flash check"):
//!
//! ```text
//! 000400 ff ff ff ff df ce ff ff fc ff ff 00 ff ff ff 00
//! 000410 fc ff ff 00 ff ff ff 00 fc ff ff 00 ff ff ff 00
//! 000420 fc ff 0f 20 ff ff 0f 20 fc ff 7f 40 ff ff 7f 40
//! 000430 fc ff 0d 40 ff ff 0d 40 ff ff ff ff ff ff ff ff
//! ```
//!
//! Placement: `bootloader/memory.x` pins the `.ofs` output section to
//! `ORIGIN(FLASH) + 0x400` and moves `_stext` to `0x440`; linker ASSERTs
//! there enforce both the address and the 64-byte size.
//!
//! ID code (OSIS, `0x0101_0018`, 0x20 bytes): must stay all-`0xFF` or
//! SWD/ROM-boot recovery locks out (SPEC §1.5). It is deliberately NOT
//! emitted here: probe-rs' R7FA4M1AB target maps that region as
//! non-flashable (`ConfigROM`, `!Generic`), so a loadable section there
//! would break `cargo run`. All-`0xFF` is the factory/erased state, and
//! nothing in this project erases or programs the OSIS region — the
//! requirement holds by leaving it untouched (stock hex ships the same
//! all-`0xFF` bytes).

/// OFS0 (`0x400`) — all ones: IWDT and WDT auto-start disabled
/// (R01UM0007, Option-Setting Memory chapter; SPEC §1.5).
const OFS0_WDT_IWDT_DISABLED: u32 = 0xFFFF_FFFF;

/// OFS1 (`0x404`) — voltage detector 0 disabled, HOCO auto-start enabled at
/// 48 MHz (HOCOEN = 0, HOCOFRQ1 = 0b100). `crates/ra4m1-embassy/src/clocks.rs`
/// relies on this HOCO auto-start.
const OFS1_HOCO_48MHZ: u32 = 0xFFFF_CEDF;

/// Security-MPU region registers (`0x408..0x438`): stock/default start and
/// end addresses with every region's enable bit (start-word bit 31... left
/// at the erased default) — the Security MPU is not used. Values are the
/// stock pairs for PC regions 0-1 and data regions 0-3 (flash, SRAM, and
/// the two security-function windows).
const SECMPU_PC0_START: u32 = 0x00FF_FFFC;
const SECMPU_PC0_END: u32 = 0x00FF_FFFF;
const SECMPU_PC1_START: u32 = 0x00FF_FFFC;
const SECMPU_PC1_END: u32 = 0x00FF_FFFF;
const SECMPU_REGION0_START: u32 = 0x00FF_FFFC;
const SECMPU_REGION0_END: u32 = 0x00FF_FFFF;
const SECMPU_REGION1_START: u32 = 0x200F_FFFC;
const SECMPU_REGION1_END: u32 = 0x200F_FFFF;
const SECMPU_REGION2_START: u32 = 0x407F_FFFC;
const SECMPU_REGION2_END: u32 = 0x407F_FFFF;
const SECMPU_REGION3_START: u32 = 0x400D_FFFC;
const SECMPU_REGION3_END: u32 = 0x400D_FFFF;

/// SECMPUAC (`0x438`) — all ones: every Security-MPU region disabled.
const SECMPU_ACCESS_CONTROL: u32 = 0xFFFF_FFFF;

/// `0x43C` — reserved, erased state.
const OFS_RESERVED: u32 = 0xFFFF_FFFF;

/// The 16 option words at `0x400..0x440`, little-endian in flash.
/// `#[used]` keeps the static through codegen/LTO; the linker `KEEP` in
/// `memory.x` keeps the section through `--gc-sections`.
#[link_section = ".ofs"]
#[used]
pub static OPTION_SETTING_WORDS: [u32; 16] = [
    OFS0_WDT_IWDT_DISABLED, // 0x400
    OFS1_HOCO_48MHZ,        // 0x404
    SECMPU_PC0_START,       // 0x408
    SECMPU_PC0_END,         // 0x40C
    SECMPU_PC1_START,       // 0x410
    SECMPU_PC1_END,         // 0x414
    SECMPU_REGION0_START,   // 0x418
    SECMPU_REGION0_END,     // 0x41C
    SECMPU_REGION1_START,   // 0x420
    SECMPU_REGION1_END,     // 0x424
    SECMPU_REGION2_START,   // 0x428
    SECMPU_REGION2_END,     // 0x42C
    SECMPU_REGION3_START,   // 0x430
    SECMPU_REGION3_END,     // 0x434
    SECMPU_ACCESS_CONTROL,  // 0x438
    OFS_RESERVED,           // 0x43C
];
