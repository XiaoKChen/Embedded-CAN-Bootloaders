//! I2C bus master over the dedicated RIIC (IIC) block (M3.3, plan §4.2/§4.3).
//!
//! Blocking [`embedded_hal::i2c::I2c`] plus async
//! [`embedded_hal_async::i2c::I2c`] (`SevenBitAddress`), driven by the RIIC's
//! own TXI/RXI/TEI/EEI interrupts and a waker, bound through the M3.0
//! [`crate::bind_interrupts`] macro. The bit-rate registers (ICBRL/ICBRH +
//! ICMR1.CKS, sourced off `PCLKB` — [`crate::clock::Clock::PclkB`]) are a pure
//! `const fn`, compile-time-checked against golden vectors below.
//!
//! # Spike S-C pattern (shared with [`crate::sci`])
//!
//! This mirrors, register-for-register, the async pattern the SCI/UART driver
//! established (see the `sci` module header): **per-transfer, interrupt-driven,
//! ISR masks-its-own-enable-bit / future acts**. The DTC is not used.
//!
//! 1. One [`WakerCell`] per RIIC instance (a dependency-free
//!    `cortex_m::interrupt::free`-guarded `Option<Waker>` — the HAL has no
//!    `embassy-sync` dependency, so it uses the same primitive as
//!    [`crate::sci`]/[`crate::adc`], not a `Signal`). I2C transactions are
//!    strictly sequential/half-duplex, so a single shared waker serves every
//!    event (unlike the SCI's rx/tx split).
//! 2. Four minimal ISRs ([`RxInterruptHandler`], [`TxInterruptHandler`],
//!    [`TeiInterruptHandler`], [`EeiInterruptHandler`]) each **mask their own
//!    ICIER enable bit(s)** (so the level request cannot storm the NVIC before
//!    the task runs), **wake** the shared waker, and return. The
//!    `bind_interrupts!`-generated vector has already cleared `IELSR.IR`.
//! 3. Every wait is a [`core::future::poll_fn`] ([`I2c::wait_async`]): read
//!    `ICSR2`, act if the flag is set (or a bus error is pending), else register
//!    the waker, **re-enable** the relevant ICIER bit(s), re-check once to close
//!    the enable/arrival race, and return `Pending`.
//! 4. Errors ride the same status reads in-band: NACK is `ICSR2.NACKF`,
//!    arbitration loss `ICSR2.AL`, bus timeout `ICSR2.TMOF`.
//!
//! **The blocking trait reuses the exact same async engine** via a tiny
//! dependency-free [`block_on`] (busy-poll with a no-op waker) — one audited
//! transfer implementation, no forked register sequencing (rules/principles.md
//! "fork intentionally"). Blocking usage therefore busy-polls; **never call the
//! blocking API from an Embassy task** (rules/embassy.md) — use the async trait.
//!
//! # RIIC init + bit-rate recipe (R01UM0007, "I2C Bus Interface (IIC)" chapter)
//!
//! `new()` performs the documented master bring-up:
//!
//! | Step | Register / bit | Purpose |
//! |---|---|---|
//! | Module-start | `MSTP.MSTPCRB.MSTPBn` = 0 | clock the block (IIC0 = bit 9, IIC1 = bit 8) |
//! | Enter reset | `ICCR1`: ICE = 0, IICRST = 1 | RIIC internal reset |
//! | Enable | `ICCR1.ICE` = 1 | enable module (still in internal reset) |
//! | Master, no slave | `ICSER` = 0 | disable slave-address / general-call match |
//! | Ref clock | `ICMR1.CKS` | IICφ = PCLKB / 2^CKS |
//! | Bit rate | `ICBRL`, `ICBRH` | SCL low/high period counts |
//! | Arm none | `ICIER` = 0 | interrupts masked until a future arms them |
//! | Release | `ICCR1.IICRST` = 0 | leave internal reset; bus usable |
//!
//! Bit-rate formula (R01UM0007, IIC "Bit Rate"): with IICφ = PCLKB / 2^CKS and
//! the SCL low/high widths counted as `(BRL + 1)` / `(BRH + 1)` IICφ cycles
//! (ignoring rise/fall and internal delays),
//! `bit_rate ≈ IICφ / (BRL + BRH + 2)`. [`compute_bitrate`] scans CKS 0..=7 for
//! the lowest-error fit with each 5-bit count in `1..=31`, biasing the low
//! period slightly longer so `tLOW ≥ tHIGH` (the I2C requirement). The real
//! RIIC adds a few fixed cycles per period, so silicon runs marginally slower
//! than the ideal (safe direction) — **BENCH-VERIFY** the exact SCL frequency.
//!
//! # ICU event numbers — BENCH-VERIFY (parent, via M2.5/Renode or silicon)
//!
//! The RXI/TXI/TEI/EEI ICU event numbers below are the FSP contiguous IIC
//! ordering, placed after USBFS (confirmed `0x033` on silicon in this repo's
//! `bootloader::drivers::usbfs`). They have **not** been silicon-checked here
//! (same status as [`crate::sci`]'s SCI events). Confirm against R01UM0007
//! (ICU event table) before trusting on hardware.
//!
//! # Open-drain SCL/SDA
//!
//! I2C requires open-drain pins. [`crate::pfs::set_peripheral_function`] routes
//! the IIC function (PSEL) but does not expose PmnPFS.NCODR, so [`I2c::new`]
//! additionally sets NCODR through a small contained MMIO helper
//! ([`set_open_drain`]) using the same documented PmnPFS map as `pfs.rs`. Folding
//! an open-drain variant into `pfs.rs` (base crate) is the tidier follow-up.
//!
//! # PAC coverage (spike S-B)
//!
//! Fully covered. `IIC0`/`IIC1` deref to `iic0`/`iic1` register blocks with an
//! identical layout for every register this driver touches; the driver views
//! both through the [`ra4m1::iic0::RegisterBlock`] superset type (see
//! [`Instance::BASE`]), exactly as [`crate::sci`] views its channels through
//! `sci0`.

use core::cell::UnsafeCell;
use core::future::{poll_fn, Future};
use core::marker::PhantomData;
use core::pin::pin;
use core::task::{Context, Poll, RawWaker, RawWakerVTable, Waker};

use embedded_hal::i2c::{
    self, ErrorKind, ErrorType, NoAcknowledgeSource, Operation, SevenBitAddress,
};
use ra4m1_embassy::pfs;

use crate::clock::Clock;
use crate::interrupt::InterruptHandler;

/// The RIIC register-block superset view (see [`Instance::BASE`]).
type Regs = ra4m1::iic0::RegisterBlock;
/// The RIIC status-register-2 read proxy, used by the wait closures.
type Icsr2 = ra4m1::iic0::icsr2::R;

/// Standard-mode I2C bit rate (100 kHz).
pub const STANDARD_MODE_HZ: u32 = 100_000;
/// Fast-mode I2C bit rate (400 kHz).
pub const FAST_MODE_HZ: u32 = 400_000;

/// PmnPFS.NCODR — N-channel open-drain control (R01UM0007, I/O Ports, PmnPFS):
/// 1 = open-drain output, required for I2C SCL/SDA.
const PFS_NCODR: u32 = 1 << 6;
/// PmnPFS register stride (R01UM0007, I/O Ports register map) — mirrors
/// `pfs.rs`: PFS base + 0x40 * port + 4 * pin.
const PFS_PORT_STRIDE: usize = 0x40;
const PFS_PIN_STRIDE: usize = 0x04;

/// Bounded busy-poll ceiling for the bus-free (BBSY) check. The bus is free at
/// the start of a well-behaved transaction; a stuck bus surfaces as
/// [`Error::Bus`] instead of a hang (rules/principles.md "fail fast").
const BUS_FREE_POLLS: u32 = 100_000;

// =============================================================================
// Errors
// =============================================================================

/// An I2C master transfer error (R01UM0007, IIC status flags). Maps to
/// [`embedded_hal::i2c::ErrorKind`] for generic drivers.
#[derive(Clone, Copy, Debug, PartialEq, Eq, defmt::Format)]
#[non_exhaustive]
pub enum Error {
    /// The addressed device did not acknowledge its address (`ICSR2.NACKF` at
    /// the address phase) — device absent or not ready.
    AddressNack,
    /// The device stopped acknowledging data (`ICSR2.NACKF` mid-write).
    DataNack,
    /// Arbitration was lost to another master (`ICSR2.AL`).
    ArbitrationLoss,
    /// A bus error or bus-busy/timeout condition (`ICSR2.TMOF`, or BBSY never
    /// cleared, or an invalid 7-bit address).
    Bus,
}

impl i2c::Error for Error {
    fn kind(&self) -> ErrorKind {
        match self {
            Error::AddressNack => ErrorKind::NoAcknowledge(NoAcknowledgeSource::Address),
            Error::DataNack => ErrorKind::NoAcknowledge(NoAcknowledgeSource::Data),
            Error::ArbitrationLoss => ErrorKind::ArbitrationLoss,
            Error::Bus => ErrorKind::Bus,
        }
    }
}

// =============================================================================
// Configuration + bit-rate arithmetic
// =============================================================================

/// I2C master configuration. Build with [`Config::standard`] (100 kHz) /
/// [`Config::fast`] (400 kHz), or [`Config::new`] for an arbitrary rate.
#[derive(Clone, Copy, Debug, PartialEq, Eq, defmt::Format)]
pub struct Config {
    /// Target SCL frequency in Hz.
    pub frequency_hz: u32,
}

impl Config {
    /// An arbitrary SCL frequency (Hz). Must be reachable from PCLKB — see
    /// [`I2c::new`]'s panic contract.
    #[must_use]
    pub const fn new(frequency_hz: u32) -> Self {
        Self { frequency_hz }
    }

    /// Standard-mode I2C (100 kHz).
    #[must_use]
    pub const fn standard() -> Self {
        Self::new(STANDARD_MODE_HZ)
    }

    /// Fast-mode I2C (400 kHz).
    #[must_use]
    pub const fn fast() -> Self {
        Self::new(FAST_MODE_HZ)
    }
}

/// Resolved RIIC bit-rate settings for a `(PCLKB, frequency)` pair.
#[derive(Clone, Copy)]
struct BitRate {
    /// Whether any in-range setting was found.
    valid: bool,
    /// ICMR1.CKS reference-clock select (IICφ = PCLKB / 2^CKS), 0..=7.
    cks: u8,
    /// ICBRL low-level period count (1..=31).
    brl: u8,
    /// ICBRH high-level period count (1..=31).
    brh: u8,
    /// Absolute frequency error of the chosen setting, in ppm.
    err_ppm: u32,
}

/// Compute ICMR1.CKS / ICBRL / ICBRH for `frequency` off `pclkb` (PCLKB).
///
/// Formula (R01UM0007, IIC "Bit Rate"): IICφ = PCLKB / 2^CKS, and
/// `bit_rate ≈ IICφ / (BRL + BRH + 2)` (SCL low/high widths of `BRL + 1` /
/// `BRH + 1` IICφ cycles, ignoring rise/fall + internal delays). Scans CKS
/// 0..=7 for the lowest-error fit with each 5-bit count in `1..=31`, biasing the
/// low period ~53.5 % so `tLOW ≥ tHIGH` (the I2C duty requirement).
///
/// Pure and `const`: the golden-vector `const _` assertions below verify it at
/// compile time (this crate is `#![no_std]`, no host test harness).
const fn compute_bitrate(pclkb: u32, frequency: u32) -> BitRate {
    let mut best = BitRate {
        valid: false,
        cks: 0,
        brl: 0,
        brh: 0,
        err_ppm: u32::MAX,
    };
    if frequency == 0 {
        return best;
    }
    let pclkb = pclkb as u64;
    let freq = frequency as u64;
    let mut cks: u32 = 0;
    while cks < 8 {
        let iicphi = pclkb >> cks; // PCLKB / 2^cks
        let total = (iicphi + freq / 2) / freq; // round(IICφ / freq) = BRL+BRH+2
                                                // Each count is 5 bits (1..=31); the sum lands in 2..=62, so total 4..=64.
        if total >= 4 && total <= 64 {
            let sum = total - 2; // BRL + BRH
            let mut brl = (sum * 107).div_ceil(200); // ceil(sum * 0.535), low-biased
            if brl > 31 {
                brl = 31;
            }
            if brl < 1 {
                brl = 1;
            }
            let brh = sum - brl;
            if brh >= 1 && brh <= 31 {
                let actual = iicphi / (brl + brh + 2);
                let ppm = actual.abs_diff(freq) * 1_000_000 / freq;
                let err_ppm = if ppm > u32::MAX as u64 {
                    u32::MAX
                } else {
                    ppm as u32
                };
                if !best.valid || err_ppm < best.err_ppm {
                    best = BitRate {
                        valid: true,
                        cks: cks as u8,
                        brl: brl as u8,
                        brh: brh as u8,
                        err_ppm,
                    };
                }
            }
        }
        cks += 1;
    }
    best
}

// Golden-vector checks (compile-time; PCLKB = 24 MHz on this board).
const _: () = {
    let d = compute_bitrate(24_000_000, STANDARD_MODE_HZ);
    assert!(d.valid, "100 kHz I2C unreachable from 24 MHz PCLKB");
    assert!(d.err_ppm < 5_000, "100 kHz I2C error exceeds 0.5%");
};
const _: () = {
    let d = compute_bitrate(24_000_000, FAST_MODE_HZ);
    assert!(d.valid, "400 kHz I2C unreachable from 24 MHz PCLKB");
    assert!(d.err_ppm < 5_000, "400 kHz I2C error exceeds 0.5%");
};

// =============================================================================
// Per-instance async state (waker cell) — the S-C primitive
// =============================================================================

/// A single-slot waker cell: registered by a transfer's future, fired from a
/// bound ISR. A dependency-free stand-in for `embassy_sync::AtomicWaker`, made
/// sound on this single-core part by taking a PRIMASK critical section around
/// every access via [`cortex_m::interrupt::free`] — the ISR cannot preempt a
/// section that has interrupts disabled. Identical to [`crate::sci`]'s cell.
struct WakerCell {
    waker: UnsafeCell<Option<Waker>>,
}

// SAFETY: every read/write of `waker` happens inside `cortex_m::interrupt::free`
// (interrupts globally masked). On a single core that makes access to the
// `UnsafeCell` mutually exclusive between thread mode and any ISR.
unsafe impl Sync for WakerCell {}

impl WakerCell {
    const fn new() -> Self {
        Self {
            waker: UnsafeCell::new(None),
        }
    }

    /// Store `w` to be woken later (called from a transfer's poll).
    fn register(&self, w: &Waker) {
        cortex_m::interrupt::free(|_| {
            // SAFETY: interrupts masked → exclusive access (see type SAFETY).
            let slot = unsafe { &mut *self.waker.get() };
            *slot = Some(w.clone());
        });
    }

    /// Wake and clear the stored waker (called from an ISR).
    fn wake(&self) {
        cortex_m::interrupt::free(|_| {
            // SAFETY: interrupts masked → exclusive access (see type SAFETY).
            let slot = unsafe { &mut *self.waker.get() };
            if let Some(w) = slot.take() {
                w.wake();
            }
        });
    }
}

/// Per-RIIC-instance async state: one shared waker (transfers are sequential).
pub struct I2cState {
    waker: WakerCell,
}

impl I2cState {
    const fn new() -> Self {
        Self {
            waker: WakerCell::new(),
        }
    }
}

// =============================================================================
// Instance trait + concrete RIIC markers
// =============================================================================

mod sealed {
    pub trait Sealed {}
}

/// A concrete RIIC channel this driver can drive. Sealed — implemented only for
/// the [`Iic0`]/[`Iic1`] markers below.
pub trait Instance: sealed::Sealed {
    /// The PAC singleton whose ownership proves the caller owns this channel.
    type Pac;
    /// Base of this channel's register block, viewed through the `iic0` superset
    /// layout (see the module docs on the iic0/iic1 layout match).
    const BASE: *const Regs;
    /// RXI (receive-data-full) ICU event number. BENCH-VERIFY (module docs).
    const RXI_EVENT: u32;
    /// TXI (transmit-data-empty) ICU event number. BENCH-VERIFY.
    const TXI_EVENT: u32;
    /// TEI (transmit-end) ICU event number. BENCH-VERIFY.
    const TEI_EVENT: u32;
    /// EEI (event/error: START/STOP/NACK/AL/TMO) ICU event number. BENCH-VERIFY.
    const EEI_EVENT: u32;
    /// MSTPCRB bit that clocks this channel (0 = running). R01UM0007, Low Power
    /// Modes chapter: IIC0 = 9, IIC1 = 8.
    const MSTP_BIT: u8;
    /// This channel's `'static` async state.
    fn state() -> &'static I2cState;
}

/// Register-block view for instance `T` (see [`Instance::BASE`]).
#[inline]
fn regs<T: Instance>() -> &'static Regs {
    // SAFETY: `T::BASE` is the documented base of an RIIC register block; the
    // `iic0`/`iic1` blocks are layout-identical for every register accessed
    // here (module docs). The owning `I2c<T>` holds `T::Pac`, so no other typed
    // access aliases this block. Shared-reference MMIO is the PAC model.
    unsafe { &*T::BASE }
}

macro_rules! impl_instance {
    ($marker:ident, $pac:path, $ptr:path, $state:ident, $mstp:expr, $rxi:expr, $txi:expr, $tei:expr, $eei:expr) => {
        #[doc = concat!("Marker for the ", stringify!($marker), " channel.")]
        pub struct $marker;
        impl sealed::Sealed for $marker {}
        static $state: I2cState = I2cState::new();
        impl Instance for $marker {
            type Pac = $pac;
            const BASE: *const Regs = <$ptr>::PTR as *const Regs;
            const RXI_EVENT: u32 = $rxi;
            const TXI_EVENT: u32 = $txi;
            const TEI_EVENT: u32 = $tei;
            const EEI_EVENT: u32 = $eei;
            const MSTP_BIT: u8 = $mstp;
            fn state() -> &'static I2cState {
                &$state
            }
        }
    };
}

// ICU event numbers: FSP contiguous IIC ordering after USBFS (0x033). BENCH-VERIFY.
impl_instance!(
    Iic0,
    ra4m1::IIC0,
    ra4m1::IIC0,
    IIC0_STATE,
    9,
    0x035,
    0x036,
    0x037,
    0x038
);
impl_instance!(
    Iic1,
    ra4m1::IIC1,
    ra4m1::IIC1,
    IIC1_STATE,
    8,
    0x03A,
    0x03B,
    0x03C,
    0x03D
);

// =============================================================================
// Interrupt handlers (the S-C mask-in-ISR / act-in-future pattern)
// =============================================================================

/// RXI (receive-data-full) handler for instance `T`, bound to `IELn` slot
/// `SLOT`. Masks ICIER.RIE and wakes the shared waker.
pub struct RxInterruptHandler<T: Instance, const SLOT: usize>(PhantomData<T>);
/// TXI (transmit-data-empty) handler for `T`. Masks ICIER.TIE, wakes.
pub struct TxInterruptHandler<T: Instance, const SLOT: usize>(PhantomData<T>);
/// TEI (transmit-end) handler for `T`. Masks ICIER.TEIE, wakes.
pub struct TeiInterruptHandler<T: Instance, const SLOT: usize>(PhantomData<T>);
/// EEI (event/error: START/STOP/NACK/AL/TMO) handler for `T`. Masks the whole
/// event-enable group in ICIER, wakes.
pub struct EeiInterruptHandler<T: Instance, const SLOT: usize>(PhantomData<T>);

// SAFETY (all four): `on_interrupt` is minimal per rules/embassy.md — one MMIO
// modify masking this event's enable bit(s), then a waker wake, then return. The
// generated vector has already cleared `IELSR.IR` for `SLOT`.
unsafe impl<T: Instance, const SLOT: usize> InterruptHandler for RxInterruptHandler<T, SLOT> {
    const SLOT: usize = SLOT;
    fn on_interrupt() {
        regs::<T>().icier.modify(|_, w| w.rie()._0());
        T::state().waker.wake();
    }
}

unsafe impl<T: Instance, const SLOT: usize> InterruptHandler for TxInterruptHandler<T, SLOT> {
    const SLOT: usize = SLOT;
    fn on_interrupt() {
        regs::<T>().icier.modify(|_, w| w.tie()._0());
        T::state().waker.wake();
    }
}

unsafe impl<T: Instance, const SLOT: usize> InterruptHandler for TeiInterruptHandler<T, SLOT> {
    const SLOT: usize = SLOT;
    fn on_interrupt() {
        regs::<T>().icier.modify(|_, w| w.teie()._0());
        T::state().waker.wake();
    }
}

unsafe impl<T: Instance, const SLOT: usize> InterruptHandler for EeiInterruptHandler<T, SLOT> {
    const SLOT: usize = SLOT;
    fn on_interrupt() {
        regs::<T>().icier.modify(|_, w| {
            w.spie()
                ._0()
                .stie()
                ._0()
                .nakie()
                ._0()
                .alie()
                ._0()
                .tmoie()
                ._0()
        });
        T::state().waker.wake();
    }
}

// ICIER arm helpers (non-capturing → coerce to `fn` pointers). Each enables the
// bit a wait parks on plus the error/event bits so a NACK/AL/TMO also wakes.
fn arm_tdre(r: &Regs) {
    r.icier
        .modify(|_, w| w.tie()._1().nakie()._1().alie()._1().tmoie()._1());
}
fn arm_tend(r: &Regs) {
    r.icier
        .modify(|_, w| w.teie()._1().nakie()._1().alie()._1().tmoie()._1());
}
fn arm_rdrf(r: &Regs) {
    r.icier
        .modify(|_, w| w.rie()._1().nakie()._1().alie()._1().tmoie()._1());
}
fn arm_stop(r: &Regs) {
    r.icier
        .modify(|_, w| w.spie()._1().alie()._1().tmoie()._1());
}

// =============================================================================
// Pin routing
// =============================================================================

/// An SCL/SDA pin to route to the RIIC, as a `(port, pin)` coordinate plus the
/// 5-bit PSEL peripheral-select code (R01UM0007, I/O Ports "Selecting
/// Peripheral Functions" — IIC pins), supplied by the caller as in
/// [`crate::sci::Pin`].
#[derive(Clone, Copy, Debug, PartialEq, Eq, defmt::Format)]
pub struct Pin {
    /// Port index 0..=9.
    pub port: u8,
    /// Pin index 0..=15.
    pub pin: u8,
    /// 5-bit PSEL peripheral-function code for the IIC function on this pin.
    pub psel: u8,
}

/// Set PmnPFS.NCODR (open-drain) on an already-routed IIC pin, preserving PSEL
/// and PMR. Contained MMIO exception (rules/embedded.md) because
/// [`crate::pfs`] does not expose NCODR — same PWPR unlock + PmnPFS address map
/// as `pfs.rs` (R01UM0007, I/O Ports).
fn set_open_drain(_pfs: &ra4m1::PFS, pmisc: &ra4m1::PMISC, port: u8, pin: u8) {
    // PWPR unlock (clear B0WI, set PFSWE), mirroring `pfs::with_pfs_unlocked`.
    pmisc.pwpr.write(|w| w.b0wi()._0().pfswe()._0());
    pmisc.pwpr.write(|w| w.b0wi()._0().pfswe()._1());
    let byte_offset = usize::from(port) * PFS_PORT_STRIDE + usize::from(pin) * PFS_PIN_STRIDE;
    // SAFETY: address derived from the PAC's PFS base via the documented PmnPFS
    // map (R01UM0007, I/O Ports); the offset stays inside the PFS block for the
    // caller's port/pin. Read-modify-write of one PmnPFS word (4-byte aligned)
    // under the PWPR unlock; `_pfs` proves ownership of the PFS singleton so no
    // concurrent typed access aliases it.
    unsafe {
        let reg = ra4m1::PFS::PTR
            .cast::<u8>()
            .add(byte_offset)
            .cast::<u32>()
            .cast_mut();
        let cur = reg.read_volatile();
        reg.write_volatile(cur | PFS_NCODR);
    }
    // Re-lock (clear PFSWE, set B0WI).
    pmisc.pwpr.write(|w| w.b0wi()._0().pfswe()._0());
    pmisc.pwpr.write(|w| w.b0wi()._1().pfswe()._0());
}

// =============================================================================
// I2c driver
// =============================================================================

/// An I2C bus master on RIIC channel `T`.
///
/// Blocking [`embedded_hal::i2c::I2c`] works immediately after [`I2c::new`]. The
/// async [`embedded_hal_async::i2c::I2c`] additionally requires the channel's
/// RXI/TXI/TEI/EEI lines to be bound with [`crate::bind_interrupts`] and routed
/// with [`I2c::enable_interrupts`].
pub struct I2c<T: Instance> {
    _iic: T::Pac,
}

impl<T: Instance> I2c<T> {
    /// Configure RIIC channel `T` as an I2C master at `config`, routing
    /// `scl`/`sda` as open-drain IIC pins.
    ///
    /// Consumes the PAC singleton so the channel is constructed exactly once.
    /// Enables the module clock, runs the RIIC reset/init sequence, and programs
    /// the bit rate. Interrupts are left masked — call [`I2c::enable_interrupts`]
    /// to use the async trait.
    ///
    /// # Panics
    ///
    /// Panics (init-time) if `config.frequency_hz` cannot be reached from PCLKB
    /// within the bit-rate register range — an unachievable rate is a
    /// configuration bug, surfaced loudly at construction (rules/principles.md).
    #[must_use]
    pub fn new(
        iic: T::Pac,
        pfs_reg: &ra4m1::PFS,
        pmisc: &ra4m1::PMISC,
        scl: Pin,
        sda: Pin,
        config: Config,
    ) -> Self {
        let br = compute_bitrate(Clock::PclkB.hz(), config.frequency_hz);
        assert!(br.valid, "I2C bit rate unachievable from PCLKB");

        enable_clock::<T>();

        let r = regs::<T>();
        // Enter internal reset (ICE = 0, IICRST = 1), then enable the module
        // while still in internal reset (R01UM0007, IIC "Initial Settings").
        r.iccr1.modify(|_, w| w.ice()._0().iicrst()._1());
        r.iccr1.modify(|_, w| w.ice()._1());

        // Master only: no slave-address / general-call match.
        // SAFETY: whole-register write of 0 (all match enables off).
        r.icser.write(|w| unsafe { w.bits(0) });

        // Bit-rate reference clock select (IICφ = PCLKB / 2^CKS). The 3-bit CKS
        // field accepts every value 0..=7, so its typed writer is safe.
        r.icmr1.modify(|_, w| w.cks().bits(br.cks));
        // SCL low/high period counts. The 5-bit fields sit in bits [4:0]; bits
        // [7:5] read/write as 1 (R01UM0007, IIC ICBRL/ICBRH).
        // SAFETY: whole-register writes with reserved high bits set to 1.
        r.icbrl.write(|w| unsafe { w.bits(0xE0 | br.brl) });
        r.icbrh.write(|w| unsafe { w.bits(0xE0 | br.brh) });

        // No interrupts armed until a future enables them.
        // SAFETY: whole-register write of 0 (all enables off).
        r.icier.write(|w| unsafe { w.bits(0) });

        // Release the internal reset — the bus is now usable.
        r.iccr1.modify(|_, w| w.iicrst()._0());

        // Route the pins to the IIC function, then make them open-drain.
        pfs::set_peripheral_function(pfs_reg, pmisc, scl.port, scl.pin, scl.psel);
        pfs::set_peripheral_function(pfs_reg, pmisc, sda.port, sda.pin, sda.psel);
        set_open_drain(pfs_reg, pmisc, scl.port, scl.pin);
        set_open_drain(pfs_reg, pmisc, sda.port, sda.pin);

        defmt::debug!(
            "I2C up: target {} Hz, CKS {}, BRL {}, BRH {}, err {} ppm",
            config.frequency_hz,
            br.cks,
            br.brl,
            br.brh,
            br.err_ppm
        );

        Self { _iic: iic }
    }

    /// Route this channel's RXI/TXI/TEI/EEI ICU events to the given `IELn` slots
    /// at `priority`, and unmask them. Call once after [`crate::bind_interrupts`]
    /// has provided the strong `IELn` symbols for these slots; each slot must
    /// match the `SLOT` of its bound handler. Required before the async trait
    /// will make progress.
    pub fn enable_interrupts(
        &self,
        rxi_slot: usize,
        txi_slot: usize,
        tei_slot: usize,
        eei_slot: usize,
        priority: u8,
    ) {
        crate::interrupt::enable(rxi_slot, T::RXI_EVENT, priority);
        crate::interrupt::enable(txi_slot, T::TXI_EVENT, priority);
        crate::interrupt::enable(tei_slot, T::TEI_EVENT, priority);
        crate::interrupt::enable(eei_slot, T::EEI_EVENT, priority);
    }

    // -------------------------------------------------------------------------
    // The single transfer engine (async). The blocking trait drives it via
    // `block_on`; the async trait awaits it directly — one audited sequence.
    // -------------------------------------------------------------------------

    /// Run one `embedded-hal` transaction: (repeated-)START + address per
    /// same-direction operation run, STOP at the end, NACK on the final read
    /// byte. On any error the bus is released with a best-effort STOP.
    async fn run(&mut self, address: u8, operations: &mut [Operation<'_>]) -> Result<(), Error> {
        if operations.is_empty() {
            return Ok(());
        }
        if address > 0x7F {
            return Err(Error::Bus); // invalid 7-bit address — fail fast
        }

        let r = regs::<T>();
        // Fresh transaction: mask all interrupts, clear stale status, reset the
        // ACK/WAIT controls.
        // SAFETY: whole-register write of 0 (all interrupt enables off).
        r.icier.write(|w| unsafe { w.bits(0) });
        clear_flags::<T>();
        r.icmr3
            .modify(|_, w| w.ackwp()._1().ackbt()._0().wait()._0());

        self.wait_bus_free()?;

        let res = self.run_ops(address, operations).await;
        if res.is_err() {
            let _ = self.stop().await; // best-effort bus release
            clear_flags::<T>();
        }
        // Leave the channel quiet.
        // SAFETY: whole-register write of 0 (all interrupt enables off).
        r.icier.write(|w| unsafe { w.bits(0) });
        res
    }

    /// The START/address/data body of a transaction (STOP on the success path).
    async fn run_ops(
        &mut self,
        address: u8,
        operations: &mut [Operation<'_>],
    ) -> Result<(), Error> {
        let mut i = 0;
        let mut first = true;
        while i < operations.len() {
            let is_read = matches!(operations[i], Operation::Read(_));
            // Extent of the maximal same-direction run [i, j) — merged without a
            // repeated-START, per the transaction contract.
            let mut j = i + 1;
            while j < operations.len() && matches!(operations[j], Operation::Read(_)) == is_read {
                j += 1;
            }

            if first {
                self.start();
                first = false;
            } else {
                self.repeated_start();
            }
            let rw = u8::from(is_read);
            self.send_address((address << 1) | rw).await?;

            if is_read {
                let total: usize = operations[i..j]
                    .iter()
                    .map(|op| match op {
                        Operation::Read(buf) => buf.len(),
                        Operation::Write(_) => 0,
                    })
                    .sum();
                self.read_into(&mut operations[i..j], total).await?;
            } else {
                for op in &operations[i..j] {
                    if let Operation::Write(buf) = op {
                        self.write_bytes(buf).await?;
                    }
                }
            }
            i = j;
        }
        self.stop().await
    }

    /// Issue a START condition (ICCR2.ST). The RIIC auto-enters master-transmit;
    /// [`I2c::send_address`] then waits for TDRE.
    fn start(&self) {
        regs::<T>().iccr2.modify(|_, w| w.st()._1());
    }

    /// Issue a repeated-START condition (ICCR2.RS) between different-direction
    /// operations.
    fn repeated_start(&self) {
        regs::<T>().iccr2.modify(|_, w| w.rs()._1());
    }

    /// Transmit the addressing byte `(address << 1) | rw` and check its ACK.
    async fn send_address(&self, byte: u8) -> Result<(), Error> {
        let r = regs::<T>();
        self.wait_async(|s| s.tdre().bit(), arm_tdre).await?;
        // SAFETY: 8-bit data write to ICDRT (R01UM0007, IIC).
        r.icdrt.write(|w| unsafe { w.bits(byte) });
        // TEND reflects the address byte fully transmitted incl. its ACK bit.
        self.wait_async(|s| s.tend().bit() || s.nackf().bit(), arm_tend)
            .await?;
        if r.icsr2.read().nackf().bit() {
            return Err(Error::AddressNack);
        }
        Ok(())
    }

    /// Stream `buf` out on the (already-addressed) write phase.
    async fn write_bytes(&self, buf: &[u8]) -> Result<(), Error> {
        let r = regs::<T>();
        for &b in buf {
            // TDRE = ready for the next byte; NACKF = the previous byte NACKed.
            self.wait_async(|s| s.tdre().bit() || s.nackf().bit(), arm_tdre)
                .await?;
            if r.icsr2.read().nackf().bit() {
                return Err(Error::DataNack);
            }
            // SAFETY: 8-bit data write to ICDRT.
            r.icdrt.write(|w| unsafe { w.bits(b) });
        }
        // Ensure the final byte is fully transmitted + acknowledged.
        self.wait_async(|s| s.tend().bit() || s.nackf().bit(), arm_tend)
            .await?;
        if r.icsr2.read().nackf().bit() {
            return Err(Error::DataNack);
        }
        Ok(())
    }

    /// Receive `total` bytes into the read operations `ops`, NACKing the last
    /// byte. RIIC master-receive uses ICMR3.WAIT to hold SCL so software can set
    /// the ACK bit per byte, and a dummy ICDRR read to kick off reception
    /// (R01UM0007, IIC "Master Reception"). **BENCH-VERIFY:** the last-byte
    /// NACK / STOP timing is the classic thing to scope on silicon.
    async fn read_into(&self, ops: &mut [Operation<'_>], total: usize) -> Result<(), Error> {
        let r = regs::<T>();
        // Hold SCL after each byte's data phase so the ACK bit is software-set.
        r.icmr3.modify(|_, w| w.wait()._1());
        // Dummy read of ICDRR begins reception of the first byte (its value is
        // not data).
        let _ = r.icdrr.read();

        let mut idx = 0usize;
        for op in ops.iter_mut() {
            if let Operation::Read(buf) = op {
                for slot in buf.iter_mut() {
                    idx += 1;
                    self.wait_async(|s| s.rdrf().bit(), arm_rdrf).await?;
                    if idx == total {
                        // NACK the final byte (ACKWP gates the ACKBT write).
                        r.icmr3.modify(|_, w| w.ackwp()._1().ackbt()._1());
                    }
                    // Reading ICDRR returns the byte and releases SCL (sending
                    // the ACK/NACK, clocking the next byte if any).
                    *slot = r.icdrr.read().icdrr().bits();
                }
            }
        }

        // Restore ACK + WAIT for the next transfer.
        r.icmr3.modify(|_, w| w.ackwp()._1().ackbt()._0());
        r.icmr3.modify(|_, w| w.wait()._0());
        Ok(())
    }

    /// Issue a STOP condition and wait for it to complete, then clear the flag.
    async fn stop(&self) -> Result<(), Error> {
        let r = regs::<T>();
        r.icsr2.modify(|_, w| w.stop()._0()); // W0C: clear before requesting
        r.iccr2.modify(|_, w| w.sp()._1());
        self.wait_async(|s| s.stop().bit(), arm_stop).await?;
        r.icsr2.modify(|_, w| w.stop()._0().nackf()._0());
        Ok(())
    }

    /// Busy-poll ICCR2.BBSY until the bus is free (bounded). Bus-free is
    /// momentary and has no dedicated interrupt, so this is a short bounded poll
    /// even on the async path.
    fn wait_bus_free(&self) -> Result<(), Error> {
        let r = regs::<T>();
        for _ in 0..BUS_FREE_POLLS {
            if !r.iccr2.read().bbsy().bit() {
                return Ok(());
            }
        }
        Err(Error::Bus)
    }

    /// The S-C wait primitive: park until `cond(ICSR2)` holds, arming the ICIER
    /// bits `arm` selects and rechecking once to close the enable/arrival race.
    /// Returns early on a pending arbitration-loss / bus-timeout status.
    async fn wait_async(&self, cond: impl Fn(&Icsr2) -> bool, arm: fn(&Regs)) -> Result<(), Error> {
        poll_fn(|cx| {
            let r = regs::<T>();
            if let Some(e) = fatal_error(&r.icsr2.read()) {
                return Poll::Ready(Err(e));
            }
            if cond(&r.icsr2.read()) {
                return Poll::Ready(Ok(()));
            }
            T::state().waker.register(cx.waker());
            arm(r);
            if let Some(e) = fatal_error(&r.icsr2.read()) {
                return Poll::Ready(Err(e));
            }
            if cond(&r.icsr2.read()) {
                return Poll::Ready(Ok(()));
            }
            Poll::Pending
        })
        .await
    }
}

/// Classify a pending fatal bus condition in `ICSR2` (arbitration loss, bus
/// timeout). NACK is handled contextually by the caller (address vs data).
fn fatal_error(s: &Icsr2) -> Option<Error> {
    if s.al().bit() {
        Some(Error::ArbitrationLoss)
    } else if s.tmof().bit() {
        Some(Error::Bus)
    } else {
        None
    }
}

/// Clear the RIIC W0C status flags (STOP/START/NACKF/AL/TMOF) via [`ra4m1`]'s
/// typed bit writers — a read-modify-write clearing only these bits, never the
/// read-only data flags (R01UM0007, IIC ICSR2).
fn clear_flags<T: Instance>() {
    regs::<T>().icsr2.modify(|_, w| {
        w.stop()
            ._0()
            .start()
            ._0()
            .nackf()
            ._0()
            .al()
            ._0()
            .tmof()
            ._0()
    });
}

/// Clear instance `T`'s MSTPCRB bit (module-start). Read-modify-write of the
/// shared MSTPCRB, done at init before any concurrency (mirrors [`crate::sci`]).
fn enable_clock<T: Instance>() {
    // SAFETY: shared-reference MMIO to the MSTP block (PAC access model);
    // whole-register RMW clearing only this channel's bit; init runs
    // single-threaded before the executor starts.
    let mstp = unsafe { &*ra4m1::MSTP::PTR };
    let cur = mstp.mstpcrb.read().bits();
    mstp.mstpcrb
        .write(|w| unsafe { w.bits(cur & !(1u32 << T::MSTP_BIT)) });
    let _ = mstp.mstpcrb.read();
}

// =============================================================================
// block_on — the dependency-free bridge that lets the blocking trait reuse the
// async engine (busy-poll with a no-op waker).
// =============================================================================

static NOOP_VTABLE: RawWakerVTable = RawWakerVTable::new(
    |_| RawWaker::new(core::ptr::null(), &NOOP_VTABLE),
    |_| {},
    |_| {},
    |_| {},
);

/// Poll `fut` to completion on the current thread with a no-op waker. Used only
/// by the blocking trait: it busy-polls the RIIC status flags (the blocking-API
/// contract, rules/embassy.md — never call this from a task).
fn block_on<F: Future>(fut: F) -> F::Output {
    let mut fut = pin!(fut);
    // SAFETY: `NOOP_VTABLE`'s functions never dereference the (null) data
    // pointer, so this `Waker` is sound; it simply does nothing on `wake`.
    let waker = unsafe { Waker::from_raw(RawWaker::new(core::ptr::null(), &NOOP_VTABLE)) };
    let mut cx = Context::from_waker(&waker);
    loop {
        if let Poll::Ready(v) = fut.as_mut().poll(&mut cx) {
            return v;
        }
    }
}

// =============================================================================
// embedded-hal trait impls (blocking + async, one engine)
// =============================================================================

impl<T: Instance> ErrorType for I2c<T> {
    type Error = Error;
}

impl<T: Instance> i2c::I2c<SevenBitAddress> for I2c<T> {
    fn transaction(
        &mut self,
        address: SevenBitAddress,
        operations: &mut [Operation<'_>],
    ) -> Result<(), Self::Error> {
        block_on(self.run(address, operations))
    }
}

impl<T: Instance> embedded_hal_async::i2c::I2c<SevenBitAddress> for I2c<T> {
    async fn transaction(
        &mut self,
        address: SevenBitAddress,
        operations: &mut [Operation<'_>],
    ) -> Result<(), Self::Error> {
        self.run(address, operations).await
    }
}
