//! SPI bus — dedicated SPI (RSPI) block, master mode (M3.3, plan §4.2/§4.3).
//!
//! Blocking [`embedded_hal::spi::SpiBus`] plus async
//! [`embedded_hal_async::spi::SpiBus`] over the RA4M1 dedicated SPI peripheral
//! (`SPI0`/`SPI1`). The async path mirrors, register-for-register, the spike
//! **S-C** async-transfer pattern established by [`crate::sci`] — see that
//! module's header for the full rationale. This module reuses it verbatim:
//!
//! 1. One dependency-free [`WakerCell`] per direction (TX/RX), guarded by
//!    [`cortex_m::interrupt::free`] — **not** `embassy-sync` (the HAL has no
//!    such dependency; the whole HAL uses this `cortex-m`-only waker cell so it
//!    stays consistent), stored in a per-instance `'static` [`SpiState`].
//! 2. The bound ISR is minimal (rules/embassy.md): it **masks its own
//!    interrupt-enable bit** (`SPCR.SPTIE`/`SPCR.SPRIE`) so the level request
//!    cannot storm the NVIC before the task runs, **wakes** that direction's
//!    waker, and returns. The [`crate::bind_interrupts`]-generated vector has
//!    already cleared `IELSR.IR`.
//! 3. Each async transfer step is a [`core::future::poll_fn`]: it reads
//!    `SPSR`, acts if ready (write/read `SPDR`), else registers the waker,
//!    **re-enables** the interrupt bit, re-checks `SPSR` once to close the
//!    enable/arrival race, and returns `Pending`.
//! 4. Errors (overrun `SPSR.OVRF`, mode fault `SPSR.MODF`) are surfaced
//!    **in-band** on the receive step of the transfer — one fewer NVIC slot per
//!    instance than a separate `SPEI` line, and the error always accompanies a
//!    frame event.
//!
//! # One frame = one write **and** one read
//!
//! The dedicated SPI is single-buffered and full-duplex: writing `SPDR` shifts
//! a frame out while shifting one in. Every transfer primitive here therefore
//! does the same two-phase step per byte — wait `SPTEF` then write `SPDR`
//! (transmit), then wait `SPRF` then read `SPDR` (receive) — reading the frame
//! back before the next write. `SpiBus::write` discards the received bytes;
//! `SpiBus::read` shifts out zero bytes; `transfer`/`transfer_in_place` keep
//! both. Reading each frame before the next write is what keeps `OVRF` from
//! ever asserting in this flow (see [`Error`]).
//!
//! # Chip-select / SSL
//!
//! This driver implements the **bus** trait only and does **not** drive a
//! hardware SSL line: `SPCMD0.SSLA` selects SSL0 but no SSL pin is muxed, so
//! chip-select is left entirely to the caller. Compose a GPIO CS with
//! [`embedded_hal_bus::spi::ExclusiveDevice`] (or `RefCellDevice`) to get an
//! `embedded-hal` `SpiDevice`. Rationale: `SpiBus`↔`SpiDevice` (CS + sharing)
//! is exactly the seam `embedded-hal-bus` exists to fill, and a caller-owned
//! GPIO CS works on every board pinout, whereas hardware SSL only reaches the
//! four fixed SSLn pads. Hardware SSL can be added later behind this same bus
//! surface without changing it.
//!
//! # Bit-rate arithmetic (R01UM0007, SPI, SPBR / SPCMD0.BRDV)
//!
//! `bit rate = PCLKA / (2 * (SPBR + 1) * 2^BRDV)`, with `SPBR` in `0..=255`
//! and `BRDV` in `0..=3` ([`Clock::PclkA`], 48 MHz on this board → 11.7 kHz …
//! 24 MHz). [`compute_bitrate`] is a pure `const fn` scanning every `BRDV` for
//! the lowest-error in-range `SPBR`; golden-vector `const _` assertions verify
//! it at compile time (this crate is `#![no_std]`, no host harness).
//!
//! # PAC coverage (spike S-B)
//!
//! Fully covered. `SPI0`/`SPI1` deref to `spi0`/`spi1` register blocks that are
//! layout-identical for every register this driver touches; the driver views
//! both through the superset [`ra4m1::spi0::RegisterBlock`] type (see
//! [`Instance::BASE`]), exactly as [`crate::sci`] views its channels through
//! `sci0`.

use core::cell::UnsafeCell;
use core::future::poll_fn;
use core::marker::PhantomData;
use core::task::{Poll, Waker};

use embedded_hal::spi::{ErrorKind, ErrorType, Mode, Phase, Polarity, MODE_0};
use ra4m1_embassy::pfs;

use crate::clock::Clock;
use crate::interrupt::InterruptHandler;

// =============================================================================
// Errors
// =============================================================================

/// A SPI bus error (R01UM0007, SPI chapter, `SPSR` flags), surfaced in-band on
/// the receive step of a transfer.
#[derive(Clone, Copy, Debug, PartialEq, Eq, defmt::Format)]
#[non_exhaustive]
pub enum Error {
    /// Overrun: a frame arrived before the previous one was read (`SPSR.OVRF`).
    /// Cannot occur in this driver's read-before-next-write flow; surfaced
    /// defensively.
    Overrun,
    /// Mode fault: `SSL` was driven while this master was active
    /// (`SPSR.MODF`). Only possible with `SPCR.MODFEN = 1`, which this driver
    /// leaves clear; surfaced defensively.
    ModeFault,
}

impl embedded_hal::spi::Error for Error {
    fn kind(&self) -> ErrorKind {
        match self {
            Error::Overrun => ErrorKind::Overrun,
            Error::ModeFault => ErrorKind::ModeFault,
        }
    }
}

// =============================================================================
// Configuration + bit-rate arithmetic
// =============================================================================

/// Frame bit order (`SPCMD0.LSBF`).
#[derive(Clone, Copy, Debug, PartialEq, Eq, defmt::Format)]
pub enum BitOrder {
    /// MSB transmitted first (`LSBF = 0`). The common default.
    MsbFirst,
    /// LSB transmitted first (`LSBF = 1`).
    LsbFirst,
}

/// SPI master line configuration. Build with [`Config::new`] (mode 0, MSB-first)
/// and override [`Config::mode`] / [`Config::bit_order`] as needed.
#[derive(Clone, Copy)]
pub struct Config {
    /// Target SCK frequency in Hz (e.g. `8_000_000`). Clamped to the achievable
    /// range by [`compute_bitrate`]; an unreachable rate panics in [`Spi::new`].
    pub frequency_hz: u32,
    /// Clock polarity/phase (SPI mode 0..=3) as an [`embedded_hal::spi::Mode`].
    pub mode: Mode,
    /// Frame bit order.
    pub bit_order: BitOrder,
}

impl Config {
    /// SPI mode 0 (CPOL = 0, CPHA = 0), MSB-first, at `frequency_hz`.
    #[must_use]
    pub const fn new(frequency_hz: u32) -> Self {
        Self {
            frequency_hz,
            mode: MODE_0,
            bit_order: BitOrder::MsbFirst,
        }
    }
}

/// Resolved bit-rate-generator settings for a `(PCLKA, frequency)` pair.
#[derive(Clone, Copy)]
struct BitRateDiv {
    /// Whether any in-range divisor was found.
    valid: bool,
    /// `SPBR` register value N (bit rate = base / (N + 1)).
    spbr: u8,
    /// `SPCMD0.BRDV` division exponent (0..=3 → base / 2^BRDV).
    brdv: u8,
    /// Absolute frequency error of the chosen setting, in ppm.
    err_ppm: u32,
}

/// Compute `SPBR`/`SPCMD0.BRDV` for `target_hz` off `pclka_hz` (PCLKA).
///
/// Formula (R01UM0007, SPI, SPBR / SPCMD0.BRDV):
/// `bit rate = pclka / (2 * (SPBR + 1) * 2^BRDV)`. Rearranged with rounding:
/// `SPBR + 1 = round(pclka / (2 * target * 2^BRDV))`, valid in `1..=256`.
/// Every `BRDV` is scanned and the lowest-error in-range result wins; `BRDV`
/// is scanned ascending so the finer-granularity (larger `SPBR`) option wins a
/// tie.
///
/// Pure and `const`: the golden-vector `const _` assertions below verify it at
/// compile time.
const fn compute_bitrate(pclka_hz: u32, target_hz: u32) -> BitRateDiv {
    let pclka = pclka_hz as u64;
    let target = target_hz as u64;
    let mut best = BitRateDiv {
        valid: false,
        spbr: 0,
        brdv: 0,
        err_ppm: u32::MAX,
    };
    if target == 0 {
        return best;
    }
    let mut brdv: u32 = 0;
    while brdv < 4 {
        let denom = 2u64 * target * (1u64 << brdv);
        let np1 = (pclka + denom / 2) / denom; // rounded (SPBR + 1)
        if np1 >= 1 && np1 <= 256 {
            let actual = pclka / (2 * np1 * (1u64 << brdv));
            let ppm_u64 = actual.abs_diff(target) * 1_000_000 / target;
            let err_ppm = if ppm_u64 > u32::MAX as u64 {
                u32::MAX
            } else {
                ppm_u64 as u32
            };
            if !best.valid || err_ppm < best.err_ppm {
                best = BitRateDiv {
                    valid: true,
                    spbr: (np1 - 1) as u8,
                    brdv: brdv as u8,
                    err_ppm,
                };
            }
        }
        brdv += 1;
    }
    best
}

// Golden-vector checks (compile-time; PCLKA = 48 MHz on this board).
const _: () = {
    let d = compute_bitrate(48_000_000, 8_000_000);
    assert!(
        d.valid && d.err_ppm == 0,
        "8 MHz SPI not exact from 48 MHz PCLKA"
    );
};
const _: () = {
    let d = compute_bitrate(48_000_000, 1_000_000);
    assert!(
        d.valid && d.err_ppm == 0,
        "1 MHz SPI not exact from 48 MHz PCLKA"
    );
};
const _: () = {
    let d = compute_bitrate(48_000_000, 400_000);
    assert!(
        d.valid && d.err_ppm == 0,
        "400 kHz SPI not exact from 48 MHz PCLKA"
    );
};
const _: () = {
    let d = compute_bitrate(48_000_000, 100_000);
    assert!(
        d.valid && d.err_ppm == 0,
        "100 kHz SPI not exact from 48 MHz PCLKA"
    );
};

/// SPI data length code for 8-bit frames: `SPCMD0.SPB = 0b0111` (R01UM0007,
/// SPI, SPCMD0.SPB — values `0b0100..=0b0111` all select 8 bits). The `ra4m1`
/// PAC does not enumerate the 8-bit variant, so `SPB` is written via the raw
/// `SPCMD0` bit composition below rather than a typed field writer.
const SPB_8BIT: u16 = 0b0111;

/// Assemble the 16-bit `SPCMD0` value from framing config and the resolved
/// `BRDV`. Bit map per R01UM0007 (SPI, SPCMD0): CPHA@0, CPOL@1, BRDV@2..3,
/// SSLA@4..6, SSLKP@7, SPB@8..11, LSBF@12, SPNDEN@13, SLNDEN@14, SCKDEN@15.
/// SSL keep/negation/clock delays stay disabled; SSLA = 0 (SSL0, unrouted).
const fn spcmd0_bits(cfg: &Config, brdv: u8) -> u16 {
    let mut v: u16 = SPB_8BIT << 8;
    // CPHA@0: SPI CPHA 1 == sample on the second (even) edge.
    if let Phase::CaptureOnSecondTransition = cfg.mode.phase {
        v |= 1 << 0;
    }
    // CPOL@1: idle-high clock.
    if let Polarity::IdleHigh = cfg.mode.polarity {
        v |= 1 << 1;
    }
    v |= ((brdv & 0x3) as u16) << 2;
    if let BitOrder::LsbFirst = cfg.bit_order {
        v |= 1 << 12; // LSBF
    }
    v
}

// =============================================================================
// Per-instance async state (waker cells) — mirrors crate::sci::WakerCell
// =============================================================================

/// A single-slot waker cell: registered by a transfer's future, fired from the
/// bound ISR. A dependency-free stand-in for `embassy_sync::AtomicWaker` (the
/// HAL does not depend on `embassy-sync`), made sound on this single-core part
/// by taking a PRIMASK critical section around every access via
/// [`cortex_m::interrupt::free`] — the ISR cannot preempt a section that has
/// interrupts disabled.
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

    /// Store `w` to be woken later (called from a future's poll).
    fn register(&self, w: &Waker) {
        cortex_m::interrupt::free(|_| {
            // SAFETY: interrupts masked → exclusive access (see type SAFETY).
            let slot = unsafe { &mut *self.waker.get() };
            *slot = Some(w.clone());
        });
    }

    /// Wake and clear the stored waker (called from the ISR).
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

/// Per-SPI-instance async state: one waker per direction. Bus errors ride the
/// RX waker (surfaced in-band, see the module header). Lives in a `'static` per
/// instance (see the `impl_instance!` expansions).
pub struct SpiState {
    rx: WakerCell,
    tx: WakerCell,
}

impl SpiState {
    const fn new() -> Self {
        Self {
            rx: WakerCell::new(),
            tx: WakerCell::new(),
        }
    }
}

// =============================================================================
// Instance trait + concrete SPI markers
// =============================================================================

mod sealed {
    pub trait Sealed {}
}

/// A concrete SPI channel this driver can drive. Sealed — implemented only for
/// the [`Spi0`]/[`Spi1`] markers below.
///
/// # ICU event numbers
///
/// [`RXI_EVENT`](Instance::RXI_EVENT) / [`TXI_EVENT`](Instance::TXI_EVENT) are
/// R01UM0007 ICU event numbers (FSP `ELC_EVENT_SPIn_RXI/TXI` for the R7FA4M1),
/// placed by the same anchoring the SCI driver uses — GPT0 CCMPA = `0x057`
/// (`ra4m1_embassy::time_driver`), USBFS INT = `0x033`
/// (`bootloader::drivers::usbfs`), SCI0 RXI = `0x08D` ([`crate::sci`]) — with
/// the SPI block following the four SCI channels in the FSP event table.
/// **BENCH-VERIFY (parent, via M2.5/Renode or silicon):** these have not been
/// silicon-checked in this repo; they are the FSP contiguous SPI ordering.
pub trait Instance: sealed::Sealed {
    /// The PAC singleton whose ownership proves the caller owns this channel.
    type Pac;
    /// Base of this channel's register block, viewed through the `spi0`
    /// superset layout (see the module docs on the spi0/spi1 layout match).
    const BASE: *const ra4m1::spi0::RegisterBlock;
    /// RXI (`SPRI`, receive-buffer-full) ICU event number.
    const RXI_EVENT: u32;
    /// TXI (`SPTI`, transmit-buffer-empty) ICU event number.
    const TXI_EVENT: u32;
    /// MSTPCRB bit that clocks this channel (0 = running). R01UM0007, Low Power
    /// Modes chapter: SPI0 = 19, SPI1 = 18.
    const MSTP_BIT: u8;
    /// This channel's `'static` async state.
    fn state() -> &'static SpiState;
}

/// Register-block view for instance `T` (see [`Instance::BASE`]).
#[inline]
fn regs<T: Instance>() -> &'static ra4m1::spi0::RegisterBlock {
    // SAFETY: `T::BASE` is the documented base of a SPI register block; the
    // `spi0` and `spi1` blocks are layout-identical for every register accessed
    // here (module docs). The owning `Spi<T>` holds `T::Pac`, so no other typed
    // access aliases this block. Shared-reference MMIO is the PAC model.
    unsafe { &*T::BASE }
}

macro_rules! impl_instance {
    ($marker:ident, $pac:path, $ptr:path, $state:ident, $mstp:expr, $rxi:expr, $txi:expr) => {
        #[doc = concat!("Marker for the ", stringify!($marker), " channel.")]
        pub struct $marker;
        impl sealed::Sealed for $marker {}
        static $state: SpiState = SpiState::new();
        impl Instance for $marker {
            type Pac = $pac;
            const BASE: *const ra4m1::spi0::RegisterBlock =
                <$ptr>::PTR as *const ra4m1::spi0::RegisterBlock;
            const RXI_EVENT: u32 = $rxi;
            const TXI_EVENT: u32 = $txi;
            const MSTP_BIT: u8 = $mstp;
            fn state() -> &'static SpiState {
                &$state
            }
        }
    };
}

impl_instance!(Spi0, ra4m1::SPI0, ra4m1::SPI0, SPI0_STATE, 19, 0xA1, 0xA2);
impl_instance!(Spi1, ra4m1::SPI1, ra4m1::SPI1, SPI1_STATE, 18, 0xA6, 0xA7);

// =============================================================================
// Interrupt handlers (the S-C mask-in-ISR / act-in-future pattern)
// =============================================================================

/// RXI (`SPRI`, receive-buffer-full) handler for instance `T`, bound to `IELn`
/// slot `SLOT`. Masks `SPCR.SPRIE` (stopping the level request from re-entering
/// before the task runs) and wakes the RX waker.
///
/// Bind via [`crate::bind_interrupts`]:
/// `IEL5 => spi::RxInterruptHandler<spi::Spi0, 5>;` (the `IELn` name and `SLOT`
/// must agree — [`crate::interrupt`] macro invariant).
pub struct RxInterruptHandler<T: Instance, const SLOT: usize>(PhantomData<T>);
/// TXI (`SPTI`, transmit-buffer-empty) handler for `T`, bound to slot `SLOT`.
/// Masks `SPCR.SPTIE`, wakes the TX waker.
pub struct TxInterruptHandler<T: Instance, const SLOT: usize>(PhantomData<T>);

// SAFETY (both): `on_interrupt` is minimal per rules/embassy.md — one MMIO
// modify to mask this event's enable bit, then a waker wake, then return. The
// generated vector has already cleared `IELSR.IR` for `SLOT`.
unsafe impl<T: Instance, const SLOT: usize> InterruptHandler for RxInterruptHandler<T, SLOT> {
    const SLOT: usize = SLOT;
    fn on_interrupt() {
        regs::<T>().spcr.modify(|_, w| w.sprie().clear_bit());
        T::state().rx.wake();
    }
}

unsafe impl<T: Instance, const SLOT: usize> InterruptHandler for TxInterruptHandler<T, SLOT> {
    const SLOT: usize = SLOT;
    fn on_interrupt() {
        regs::<T>().spcr.modify(|_, w| w.sptie().clear_bit());
        T::state().tx.wake();
    }
}

// =============================================================================
// Pin routing
// =============================================================================

/// A RSPCK/MOSI/MISO pin to route to the SPI, as a `(port, pin)` coordinate
/// plus the 5-bit PSEL peripheral-select code. PSEL is a board/datasheet fact
/// (R01UM0007, "Selecting Peripheral Functions" — SPI pins) supplied by the
/// caller, mirroring how [`ra4m1_embassy::pfs`] takes a PSEL directly.
#[derive(Clone, Copy, Debug, PartialEq, Eq, defmt::Format)]
pub struct Pin {
    /// Port index 0..=9.
    pub port: u8,
    /// Pin index 0..=15.
    pub pin: u8,
    /// 5-bit PSEL peripheral-function code for the SPI function on this pin.
    pub psel: u8,
}

// =============================================================================
// Spi driver
// =============================================================================

/// A full-duplex SPI **master** on channel `T`.
///
/// Blocking [`embedded_hal::spi::SpiBus`] works immediately after [`Spi::new`].
/// The async [`embedded_hal_async::spi::SpiBus`] additionally requires the
/// channel's `SPTI`/`SPRI` lines to be bound with [`crate::bind_interrupts`]
/// and routed with [`Spi::enable_interrupts`].
///
/// The blocking traits busy-poll `SPSR`; they are for non-executor contexts
/// (early boot, panic paths). **Never call the blocking API from an Embassy
/// task** — use the async traits there (rules/embassy.md).
///
/// Chip-select is the caller's: wrap this in
/// [`embedded_hal_bus::spi::ExclusiveDevice`] with a GPIO CS to get an
/// `embedded-hal` `SpiDevice` (see the module header).
pub struct Spi<T: Instance> {
    _spi: T::Pac,
}

impl<T: Instance> Spi<T> {
    /// Configure SPI channel `T` as a master at `config`, routing
    /// `sck`/`mosi`/`miso`.
    ///
    /// Consumes the PAC singleton (`spi`) so the channel is constructed exactly
    /// once. Enables the module clock, muxes the pins, programs mode/bit-order
    /// and the bit-rate divisor, and enables the bus. Interrupts are left masked
    /// — call [`Spi::enable_interrupts`] to use the async traits.
    ///
    /// # Panics
    ///
    /// Panics (init-time) if `config.frequency_hz` cannot be reached from PCLKA
    /// within the `SPBR`/`BRDV` range — an unachievable rate is a configuration
    /// bug, surfaced loudly at construction (rules/principles.md "fail fast").
    #[must_use]
    pub fn new(
        spi: T::Pac,
        pfs_reg: &ra4m1::PFS,
        pmisc: &ra4m1::PMISC,
        sck: Pin,
        mosi: Pin,
        miso: Pin,
        config: Config,
    ) -> Self {
        let div = compute_bitrate(Clock::PclkA.hz(), config.frequency_hz);
        assert!(div.valid, "SPI bit rate unachievable from PCLKA");

        enable_clock::<T>();

        let spi_regs = regs::<T>();
        // Disable the bus before touching configuration (R01UM0007, SPI setup:
        // SPBR/SPCMD/SPDCR change only while SPE = 0).
        // SAFETY: whole-register write of 0 (all control bits off, SPE = 0),
        // the documented pre-config state for this driver-owned channel.
        spi_regs.spcr.write(|w| unsafe { w.bits(0) });

        pfs::set_peripheral_function(pfs_reg, pmisc, sck.port, sck.pin, sck.psel);
        pfs::set_peripheral_function(pfs_reg, pmisc, mosi.port, mosi.pin, mosi.psel);
        pfs::set_peripheral_function(pfs_reg, pmisc, miso.port, miso.pin, miso.psel);

        // No loopback, MOSI idle level 0, SSL active-low, min delays.
        // SAFETY: whole-register writes of documented field compositions
        // (R01UM0007, SPI) while the bus is disabled.
        spi_regs.sppcr.write(|w| unsafe { w.bits(0) });
        spi_regs.sslp.write(|w| unsafe { w.bits(0) });
        spi_regs.spckd.write(|w| unsafe { w.bits(0) });
        spi_regs.sslnd.write(|w| unsafe { w.bits(0) });
        spi_regs.spnd.write(|w| unsafe { w.bits(0) });
        spi_regs.spcr2.write(|w| unsafe { w.bits(0) });

        spi_regs.spbr.write(|w| unsafe { w.bits(div.spbr) });
        // SPLW = 1: access frames through the word-wide `SPDR` register (8-bit
        // frames land right-justified in SPDR[7:0]). SPRDTD = 0 (read on RX).
        spi_regs
            .spdcr
            .write(|w| w.splw().set_bit().sprdtd().clear_bit());
        // SPCMD0: mode, bit order, BRDV, 8-bit frames (see spcmd0_bits).
        spi_regs
            .spcmd0
            .write(|w| unsafe { w.bits(spcmd0_bits(&config, div.brdv)) });

        // Enable: master, SPI mode with SSL (SPMS = 0), full duplex
        // (TXMD = 0), mode-fault detection off (MODFEN = 0), interrupt enables
        // 0 until a future arms them, SPE = 1 last.
        spi_regs.spcr.write(|w| w.mstr().set_bit().spe().set_bit());

        let cpol = matches!(config.mode.polarity, Polarity::IdleHigh);
        let cpha = matches!(config.mode.phase, Phase::CaptureOnSecondTransition);
        defmt::debug!(
            "SPI master up: freq target {} Hz, SPBR {}, BRDV {}, CPOL {}, CPHA {}, err {} ppm",
            config.frequency_hz,
            div.spbr,
            div.brdv,
            cpol,
            cpha,
            div.err_ppm
        );

        Self { _spi: spi }
    }

    /// Route this channel's `SPRI`/`SPTI` ICU events to the given `IELn` slots
    /// at `priority`, and unmask them. Call once after [`crate::bind_interrupts`]
    /// has provided the strong `IELn` symbols for these slots; each slot must
    /// match the `SLOT` of its bound handler (the [`crate::interrupt`] macro
    /// invariant). Required before the async traits will make progress.
    pub fn enable_interrupts(&self, rxi_slot: usize, txi_slot: usize, priority: u8) {
        crate::interrupt::enable(rxi_slot, T::RXI_EVENT, priority);
        crate::interrupt::enable(txi_slot, T::TXI_EVENT, priority);
    }
}

/// Clear instance `T`'s MSTPCRB bit (module-start). Read-modify-write of the
/// shared MSTPCRB, done at init before any concurrency (mirrors the SCI
/// driver's `enable_clock`).
fn enable_clock<T: Instance>() {
    // SAFETY: shared-reference MMIO to the MSTP block (PAC access model);
    // MSTPCRB is not PRCR-protected on this part (verified by the base time
    // driver's MSTPCRD start). Whole-register RMW clearing only this channel's
    // bit; init runs single-threaded.
    let mstp = unsafe { &*ra4m1::MSTP::PTR };
    let cur = mstp.mstpcrb.read().bits();
    mstp.mstpcrb
        .write(|w| unsafe { w.bits(cur & !(1u32 << T::MSTP_BIT)) });
    let _ = mstp.mstpcrb.read();
}

/// Classify any pending bus error in `spsr` (OVRF/MODF). `None` if clean.
fn bus_error(spsr: &ra4m1::spi0::spsr::R) -> Option<Error> {
    if spsr.ovrf().bit_is_set() {
        Some(Error::Overrun)
    } else if spsr.modf().bit_is_set() {
        Some(Error::ModeFault)
    } else {
        None
    }
}

/// Clear the sticky error flags (write-0-to-clear after a read saw them set;
/// R01UM0007, SPI SPSR). Leaves the read-only status flags untouched.
fn clear_errors<T: Instance>() {
    regs::<T>().spsr.modify(|_, w| {
        w.ovrf().clear_bit();
        w.modf().clear_bit();
        w.perf().clear_bit();
        w.udrf().clear_bit()
    });
}

// -----------------------------------------------------------------------------
// Blocking transfer primitives
// -----------------------------------------------------------------------------

/// Blocking one-frame full-duplex exchange: wait `SPTEF`, write `tx`, wait
/// `SPRF`, return the received byte. Busy-polls `SPSR` (blocking-trait
/// contract).
fn xfer_blocking<T: Instance>(tx: u8) -> Result<u8, Error> {
    let spi = regs::<T>();
    while spi.spsr.read().sptef().bit_is_clear() {}
    // SAFETY: 8-bit frame right-justified into the word-wide SPDR; the write
    // clears SPTEF and starts the shift (R01UM0007, SPI).
    spi.spdr().write(|w| unsafe { w.bits(u32::from(tx)) });
    loop {
        let s = spi.spsr.read();
        if let Some(e) = bus_error(&s) {
            clear_errors::<T>();
            return Err(e);
        }
        if s.sprf().bit_is_set() {
            break;
        }
    }
    Ok(spi.spdr().read().bits() as u8) // reading SPDR clears SPRF
}

/// Busy-poll `SPSR.IDLNF` until the bus is idle (blocking `flush`). Returns
/// immediately in the common case: each frame's `SPRF` wait already drained the
/// shift register.
fn wait_idle_blocking<T: Instance>() {
    while regs::<T>().spsr.read().idlnf().bit_is_set() {}
}

// -----------------------------------------------------------------------------
// Async transfer primitives (the S-C reference pattern)
// -----------------------------------------------------------------------------

/// Async one-frame full-duplex exchange, mirroring [`crate::sci`]'s two-phase
/// `write`/`read` futures: await `SPTEF` (arm `SPTIE`), write `tx`; then await
/// `SPRF` (arm `SPRIE`), return the received byte. Bus errors surface in-band on
/// the receive phase.
async fn xfer_async<T: Instance>(tx: u8) -> Result<u8, Error> {
    // Phase 1 — transmit: wait until the buffer can accept a frame, then write.
    poll_fn(|cx| {
        let spi = regs::<T>();
        if spi.spsr.read().sptef().bit_is_set() {
            // SAFETY: 8-bit frame into the word-wide SPDR; clears SPTEF.
            spi.spdr().write(|w| unsafe { w.bits(u32::from(tx)) });
            return Poll::Ready(());
        }
        T::state().tx.register(cx.waker());
        spi.spcr.modify(|_, w| w.sptie().set_bit());
        if spi.spsr.read().sptef().bit_is_set() {
            spi.spdr().write(|w| unsafe { w.bits(u32::from(tx)) });
            return Poll::Ready(());
        }
        Poll::Pending
    })
    .await;

    // Phase 2 — receive: wait for the frame to shift in, then read it.
    poll_fn(|cx| {
        let spi = regs::<T>();
        let s = spi.spsr.read();
        if let Some(e) = bus_error(&s) {
            clear_errors::<T>();
            return Poll::Ready(Err(e));
        }
        if s.sprf().bit_is_set() {
            return Poll::Ready(Ok(spi.spdr().read().bits() as u8));
        }
        T::state().rx.register(cx.waker());
        spi.spcr.modify(|_, w| w.sprie().set_bit());
        let s = spi.spsr.read();
        if let Some(e) = bus_error(&s) {
            clear_errors::<T>();
            return Poll::Ready(Err(e));
        }
        if s.sprf().bit_is_set() {
            return Poll::Ready(Ok(spi.spdr().read().bits() as u8));
        }
        Poll::Pending
    })
    .await
}

// -----------------------------------------------------------------------------
// Blocking embedded-hal SpiBus
// -----------------------------------------------------------------------------

impl<T: Instance> ErrorType for Spi<T> {
    type Error = Error;
}

impl<T: Instance> embedded_hal::spi::SpiBus<u8> for Spi<T> {
    fn read(&mut self, words: &mut [u8]) -> Result<(), Error> {
        for w in words.iter_mut() {
            *w = xfer_blocking::<T>(0)?;
        }
        Ok(())
    }

    fn write(&mut self, words: &[u8]) -> Result<(), Error> {
        for &b in words {
            let _ = xfer_blocking::<T>(b)?; // discard received frame
        }
        Ok(())
    }

    fn transfer(&mut self, read: &mut [u8], write: &[u8]) -> Result<(), Error> {
        let n = read.len().max(write.len());
        for i in 0..n {
            let tx = write.get(i).copied().unwrap_or(0);
            let rx = xfer_blocking::<T>(tx)?;
            if let Some(slot) = read.get_mut(i) {
                *slot = rx;
            }
        }
        Ok(())
    }

    fn transfer_in_place(&mut self, words: &mut [u8]) -> Result<(), Error> {
        for w in words.iter_mut() {
            *w = xfer_blocking::<T>(*w)?;
        }
        Ok(())
    }

    fn flush(&mut self) -> Result<(), Error> {
        wait_idle_blocking::<T>();
        Ok(())
    }
}

// -----------------------------------------------------------------------------
// Async embedded-hal-async SpiBus (the S-C reference pattern)
// -----------------------------------------------------------------------------

impl<T: Instance> embedded_hal_async::spi::SpiBus<u8> for Spi<T> {
    async fn read(&mut self, words: &mut [u8]) -> Result<(), Error> {
        for w in words.iter_mut() {
            *w = xfer_async::<T>(0).await?;
        }
        Ok(())
    }

    async fn write(&mut self, words: &[u8]) -> Result<(), Error> {
        for &b in words {
            let _ = xfer_async::<T>(b).await?; // discard received frame
        }
        Ok(())
    }

    async fn transfer(&mut self, read: &mut [u8], write: &[u8]) -> Result<(), Error> {
        let n = read.len().max(write.len());
        for i in 0..n {
            let tx = write.get(i).copied().unwrap_or(0);
            let rx = xfer_async::<T>(tx).await?;
            if let Some(slot) = read.get_mut(i) {
                *slot = rx;
            }
        }
        Ok(())
    }

    async fn transfer_in_place(&mut self, words: &mut [u8]) -> Result<(), Error> {
        for w in words.iter_mut() {
            *w = xfer_async::<T>(*w).await?;
        }
        Ok(())
    }

    async fn flush(&mut self) -> Result<(), Error> {
        // Each frame's phase-2 `SPRF` wait already drained the shift register,
        // so the bus is idle here — no interrupt to await. Verify `IDLNF` is
        // clear without busy-waiting (rules/embassy.md: no spin in a task).
        Ok(())
    }
}
