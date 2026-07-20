//! PWPR-locked pin-function (PmnPFS) writes, PSEL-before-PMR ordering
//! (SPEC §5.2).
//!
//! Registers: R01UM0007 (RA4M1 HW manual), I/O Ports chapter — PmnPFS pin
//! function control registers and the PWPR write-protect register.
//!
//! The PAC types every PmnPFS register individually (`p111pfs()`, ...),
//! which cannot express a `(port, pin)`-generic driver. This module instead
//! computes each register's address from the documented map — PFS base +
//! `0x40 * port + 4 * pin` — and performs volatile whole-word accesses.
//! This is the project's one contained raw-MMIO exception
//! (rules/embedded.md); every caller still proves ownership by passing the
//! `PFS`/`PMISC` peripheral singletons.

use ra4m1::{PFS, PMISC};

/// PmnPFS.PODR — output data (1 = high). R01UM0007, PmnPFS description.
pub const PFS_PODR: u32 = 1 << 0;
/// PmnPFS.PDR — direction (1 = output).
pub const PFS_PDR: u32 = 1 << 2;
/// PmnPFS.NCODR — N-channel open-drain output (1 = open-drain). Required for
/// bidirectional peripheral pins such as I2C SCL/SDA.
pub const PFS_NCODR: u32 = 1 << 6;
/// PmnPFS.ASEL — analog input enable (1 = pin used as an analog function; the
/// digital GPIO input path is disconnected). Set for ADC inputs / the DAC output.
pub const PFS_ASEL: u32 = 1 << 15;
/// PmnPFS.PMR — pin mode (1 = peripheral function selected by PSEL).
pub const PFS_PMR: u32 = 1 << 16;
/// PmnPFS.PSEL\[4:0\] bit position (peripheral select code, e.g. 0x10 = CAN).
pub const PFS_PSEL_SHIFT: u32 = 24;

/// PmnPFS registers are laid out at PFS base + 0x40 * port + 4 * pin
/// (R01UM0007, I/O Ports register map).
const PFS_PORT_STRIDE: usize = 0x40;
const PFS_PIN_STRIDE: usize = 0x04;
/// RA4M1 (100-pin and below) exposes ports 0..=9, pins 0..=15.
const PORT_MAX: u8 = 9;
const PIN_MAX: u8 = 15;
/// PSEL is a 5-bit field.
const PSEL_MASK: u8 = 0x1F;

/// Configure `Pport_pin` as a GPIO output driving `high` (PMR = 0,
/// PDR = 1, PODR = level; every other PmnPFS field to its 0/default state).
pub fn set_gpio_output(pfs: &PFS, pmisc: &PMISC, port: u8, pin: u8, high: bool) {
    let word = PFS_PDR | if high { PFS_PODR } else { 0 };
    with_pfs_unlocked(pmisc, || {
        write_pmnpfs(pfs, port, pin, word);
    });
}

/// Route `Pport_pin` to peripheral function `psel` (5-bit PSEL code).
///
/// Ordering per R01UM0007 (and SPEC §5.2): PSEL may only change while
/// PMR = 0, so this is two writes — the full word with PMR clear, then the
/// same word with PMR set.
pub fn set_peripheral_function(pfs: &PFS, pmisc: &PMISC, port: u8, pin: u8, psel: u8) {
    let word = u32::from(psel & PSEL_MASK) << PFS_PSEL_SHIFT;
    with_pfs_unlocked(pmisc, || {
        write_pmnpfs(pfs, port, pin, word);
        write_pmnpfs(pfs, port, pin, word | PFS_PMR);
    });
}

/// Return `Pport_pin` to its reset state: GPIO input, no pull, PSEL = 0
/// (PmnPFS = 0x0000_0000). Used by CAN deinit before jump/reset (R32).
pub fn reset_to_gpio_input(pfs: &PFS, pmisc: &PMISC, port: u8, pin: u8) {
    with_pfs_unlocked(pmisc, || {
        write_pmnpfs(pfs, port, pin, 0);
    });
}

/// Configure `Pport_pin` for an analog function: PmnPFS.ASEL = 1 with every
/// other field cleared (PMR = 0 peripheral-mode off, PDR = 0 input, ISEL = 0, no
/// pull, PSEL = 0). The digital input buffer is disconnected so the analog mux
/// sees a clean pad. Used for ADC input pins and the DAC output pin
/// (R01UM0007, I/O Ports — PmnPFS.ASEL).
pub fn set_analog(pfs: &PFS, pmisc: &PMISC, port: u8, pin: u8) {
    with_pfs_unlocked(pmisc, || {
        write_pmnpfs(pfs, port, pin, PFS_ASEL);
    });
}

/// Set PmnPFS.NCODR (N-channel open-drain) on `Pport_pin`, preserving its
/// current function/PSEL/PMR (read-modify-write). Peripheral pins that need
/// open-drain — e.g. I2C SCL/SDA — call this after [`set_peripheral_function`]
/// (R01UM0007, I/O Ports — PmnPFS.NCODR).
pub fn set_open_drain(pfs: &PFS, pmisc: &PMISC, port: u8, pin: u8) {
    with_pfs_unlocked(pmisc, || {
        let word = read_pmnpfs(pfs, port, pin) | PFS_NCODR;
        write_pmnpfs(pfs, port, pin, word);
    });
}

/// Run `f` with PmnPFS writes enabled. PWPR protocol (R01UM0007, I/O Ports,
/// PWPR): B0WI guards PFSWE, so enabling takes two writes (clear B0WI, set
/// PFSWE) and re-locking mirrors them (clear PFSWE, set B0WI). Both fields
/// are written explicitly each time because `write()` would otherwise fill
/// in the reset value (B0WI = 1).
fn with_pfs_unlocked(pmisc: &PMISC, f: impl FnOnce()) {
    pmisc.pwpr.write(|w| w.b0wi()._0().pfswe()._0());
    pmisc.pwpr.write(|w| w.b0wi()._0().pfswe()._1());
    f();
    pmisc.pwpr.write(|w| w.b0wi()._0().pfswe()._0());
    pmisc.pwpr.write(|w| w.b0wi()._1().pfswe()._0());
}

/// Volatile whole-word write to PmnPFS for `Pport_pin`. Callers must hold
/// the PWPR unlock ([`with_pfs_unlocked`]) or the write is ignored.
fn write_pmnpfs(_pfs: &PFS, port: u8, pin: u8, word: u32) {
    debug_assert!(port <= PORT_MAX && pin <= PIN_MAX);
    let byte_offset = usize::from(port) * PFS_PORT_STRIDE + usize::from(pin) * PFS_PIN_STRIDE;
    // SAFETY: address derived from the PAC's PFS base via the documented
    // PmnPFS map (R01UM0007, I/O Ports); the offset stays inside the PFS
    // block for the asserted port/pin ranges. Volatile MMIO write, 4-byte
    // aligned (both strides are multiples of 4). `_pfs` proves the caller
    // owns the PFS singleton, so no concurrent typed access aliases this.
    unsafe {
        ra4m1::PFS::PTR
            .cast::<u8>()
            .add(byte_offset)
            .cast::<u32>()
            .cast_mut()
            .write_volatile(word);
    }
}

/// Volatile whole-word read of PmnPFS for `Pport_pin`. Companion to
/// [`write_pmnpfs`] for read-modify-write field updates (e.g. NCODR).
fn read_pmnpfs(_pfs: &PFS, port: u8, pin: u8) -> u32 {
    debug_assert!(port <= PORT_MAX && pin <= PIN_MAX);
    let byte_offset = usize::from(port) * PFS_PORT_STRIDE + usize::from(pin) * PFS_PIN_STRIDE;
    // SAFETY: address derived from the PAC's PFS base via the same documented
    // PmnPFS map as [`write_pmnpfs`] (R01UM0007, I/O Ports); the offset stays
    // inside the PFS block for the asserted port/pin ranges. Volatile MMIO read,
    // 4-byte aligned. `_pfs` proves the caller owns the PFS singleton.
    unsafe {
        ra4m1::PFS::PTR
            .cast::<u8>()
            .add(byte_offset)
            .cast::<u32>()
            .read_volatile()
    }
}
