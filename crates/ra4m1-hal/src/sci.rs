//! SCI serial — UART (asynchronous serial) mode (M3.3, plan §4.2/§4.5).
//!
//! Blocking [`embedded_io`] `Read`/`Write` plus async
//! [`embedded_io_async`] `Read`/`Write`, RX/TX driven by the SCI's own
//! TXI/RXI/TEI interrupts and a waker, bound through the M3.0
//! [`crate::bind_interrupts`] macro. The baud divisor (BRR + SEMR + SMR.CKS,
//! sourced off `PCLKB` — [`crate::clock::Clock::PclkB`]) is a pure `const fn`,
//! compile-time-checked against golden vectors below.
//!
//! # Spike S-C decision — per-byte, interrupt-driven (NOT the DTC)
//!
//! **Decision: per-byte, interrupt-driven transfers using the classic Embassy
//! *mask-in-ISR / act-in-future* pattern. The DTC is NOT used.** This is the
//! reference pattern the Round-2 SPI/I2C drivers (`spi.rs`, `i2c.rs`) must
//! mirror.
//!
//! Rationale:
//!
//! - **Portability across all four SCI channels.** In the `ra4m1` 0.2 PAC only
//!   `SCI0`/`SCI1` (the `sci0` block) carry a FIFO; `SCI2`/`SCI9` (the `sci2`
//!   block) have **no** FIFO registers at all. The Arduino UNO R4 Minima's
//!   `Serial1` (D0/D1) is on `SCI2` — i.e. the FIFO-less block — so a FIFO-only
//!   fast path would not serve the board's most common UART. Per-byte
//!   TDRE/RDRF interrupts are the common denominator that works on every
//!   channel with one code path.
//! - **DTC cost is not justified for a first HAL.** A DTC block transfer needs
//!   a RAM-resident transfer-descriptor table, DTCER routing per activation
//!   source, and its own completion/error interrupt wiring — a whole subsystem
//!   (plan Phase 5's "DTC/DMAC helper surface") to bring up and bench-verify.
//!   Per-byte interrupts hit the requirement (await a real hardware event, no
//!   busy-wait, rules/embassy.md) with none of that. When throughput demands it
//!   later, the DTC becomes an *optimization behind this same async surface*,
//!   not a prerequisite.
//! - **FIFO is deferred, not forbidden.** On `SCI0`/`SCI1` the FIFO could batch
//!   bytes per interrupt; that is a future refinement that does not change the
//!   async architecture (still: ISR masks the enable bit and wakes; the future
//!   drains/fills and re-arms). It is intentionally left out here so all four
//!   channels share one audited path.
//!
//! ## The pattern SPI/I2C must follow
//!
//! 1. One [`AtomicWaker`](embassy_sync::waitqueue::AtomicWaker) per direction,
//!    stored in a per-instance `'static` [`SciState`] reached through
//!    [`Instance::state`].
//! 2. The bound ISR is minimal (rules/embassy.md): it **masks its own
//!    interrupt-enable bit** (so the level-sensitive request cannot storm the
//!    NVIC before the task runs), **wakes** the direction's waker, and returns.
//!    The `bind_interrupts!`-generated vector has already cleared `IELSR.IR`.
//! 3. The async method is a [`core::future::poll_fn`]: on each poll it reads
//!    the status register, acts if ready (read/write one datum), else registers
//!    the waker, **re-enables** the interrupt bit, re-checks the status once to
//!    close the enable/arrival race, and returns `Pending`.
//! 4. Errors are surfaced **in-band** on the RX path (SSR ORER/FER/PER checked
//!    on every read) rather than via a separate ERI line — one fewer NVIC slot
//!    per instance, and the error always accompanies a received-data event.
//!
//! # PAC coverage (spike S-B)
//!
//! Fully covered. `SCI0`/`SCI1` deref to `sci0::RegisterBlock`, `SCI2`/`SCI9`
//! to `sci2::RegisterBlock`; the two share an identical layout for every
//! register this driver touches (SMR@0, BRR@1, SCR@2, TDR@3, SSR@4, RDR@5,
//! SEMR@7, MDDR@0x12 — the FIFO registers `sci2` lacks live at 0x14+ and are
//! never accessed here). The driver therefore views every instance through the
//! superset [`ra4m1::sci0::RegisterBlock`] type (see [`Instance::BASE`]).

use core::future::poll_fn;
use core::marker::PhantomData;
use core::task::Poll;

use embassy_sync::waitqueue::AtomicWaker;
use ra4m1_embassy::pfs;

use crate::clock::Clock;
use crate::interrupt::InterruptHandler;

// =============================================================================
// Errors
// =============================================================================

/// A UART receive error (R01UM0007, SCI chapter, SSR flags). Transmit has no
/// error condition in asynchronous mode.
#[derive(Clone, Copy, Debug, PartialEq, Eq, defmt::Format)]
#[non_exhaustive]
pub enum Error {
    /// Overrun: a new byte arrived in RDR before the previous one was read
    /// (SSR.ORER). Data was lost.
    Overrun,
    /// Framing error: an expected stop bit was low (SSR.FER) — usually a baud
    /// mismatch or a line glitch.
    Framing,
    /// Parity error: received parity did not match the configured mode
    /// (SSR.PER).
    Parity,
}

impl core::fmt::Display for Error {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        let s = match self {
            Error::Overrun => "SCI receive overrun",
            Error::Framing => "SCI framing error",
            Error::Parity => "SCI parity error",
        };
        f.write_str(s)
    }
}

// `embedded_io::Error` requires the `core::error::Error` supertrait (0.7).
impl core::error::Error for Error {}

impl embedded_io::Error for Error {
    fn kind(&self) -> embedded_io::ErrorKind {
        // `embedded_io::ErrorKind` has no UART-specific variants; the concrete
        // cause is carried by this enum's own `Debug`/`defmt::Format`.
        embedded_io::ErrorKind::Other
    }
}

// =============================================================================
// Configuration + baud arithmetic
// =============================================================================

/// Character length (SMR.CHR).
#[derive(Clone, Copy, Debug, PartialEq, Eq, defmt::Format)]
pub enum DataBits {
    /// 7 data bits (CHR = 1).
    Seven,
    /// 8 data bits (CHR = 0). The common default.
    Eight,
}

/// Parity mode (SMR.PE/PM).
#[derive(Clone, Copy, Debug, PartialEq, Eq, defmt::Format)]
pub enum Parity {
    /// No parity (PE = 0). The common default.
    None,
    /// Even parity (PE = 1, PM = 0).
    Even,
    /// Odd parity (PE = 1, PM = 1).
    Odd,
}

/// Stop-bit count (SMR.STOP).
#[derive(Clone, Copy, Debug, PartialEq, Eq, defmt::Format)]
pub enum StopBits {
    /// 1 stop bit (STOP = 0). The common default.
    One,
    /// 2 stop bits (STOP = 1).
    Two,
}

/// UART line configuration. Build with [`Config::new`] (8N1) and override the
/// framing fields as needed.
#[derive(Clone, Copy, Debug, PartialEq, Eq, defmt::Format)]
pub struct Config {
    /// Target bit rate in bits per second (e.g. `115_200`).
    pub baud: u32,
    /// Data-bit count.
    pub data_bits: DataBits,
    /// Parity mode.
    pub parity: Parity,
    /// Stop-bit count.
    pub stop_bits: StopBits,
}

impl Config {
    /// 8 data bits, no parity, 1 stop bit ("8N1") at `baud` bits per second.
    #[must_use]
    pub const fn new(baud: u32) -> Self {
        Self {
            baud,
            data_bits: DataBits::Eight,
            parity: Parity::None,
            stop_bits: StopBits::One,
        }
    }
}

/// Resolved bit-rate-generator settings for a `(PCLKB, baud)` pair.
#[derive(Clone, Copy)]
struct BaudDiv {
    /// Whether any in-range divisor was found.
    valid: bool,
    /// SMR.CKS clock prescale exponent (0..=3 → PCLKB / 4^cks internally).
    cks: u8,
    /// SEMR.ABCS (asynchronous base-clock select: halves the base cycles/bit).
    abcs: bool,
    /// SEMR.BGDM (baud-rate-generator double-speed: halves the base cycles/bit).
    bgdm: bool,
    /// BRR register value N (bit rate = base / (N + 1)).
    brr: u8,
    /// Absolute frequency error of the chosen setting, in ppm.
    err_ppm: u32,
}

/// Candidate `(BGDM, ABCS, base clocks-per-bit at CKS = 0)` combinations.
///
/// R01UM0007 (SCI, asynchronous bit rate): with SEMR.ABCSE = 0 and BRME = 0 the
/// base clock is 32 cycles/bit, halved by BGDM = 1 and again by ABCS = 1, and
/// scaled by 4^CKS. Listing the finer-granularity (smaller divisor) options
/// first lets the search prefer them on an exact tie, which minimises error.
const BAUD_MODES: [(bool, bool, u64); 3] = [(true, true, 8), (true, false, 16), (false, false, 32)];

/// Compute BRR/SEMR/SMR.CKS for `baud` off `pclk_hz` (PCLKB).
///
/// Formula (R01UM0007, SCI async, BRME = 0):
/// `bit_rate = pclk / (cpb * (BRR + 1))`, where `cpb` = base cycles-per-bit
/// (32, halved by BGDM and by ABCS) times `4^CKS`. Rearranged with rounding:
/// `BRR + 1 = round(pclk / (cpb * baud))`, valid when it lands in `1..=256`.
/// Every `(mode, cks)` is scanned and the lowest-error in-range result wins.
///
/// Pure and `const`: the golden-vector `const _` assertions below verify it at
/// compile time (no host test harness — this crate is `#![no_std]`).
const fn compute_baud(pclk_hz: u32, baud: u32) -> BaudDiv {
    let pclk = pclk_hz as u64;
    let baud = baud as u64;
    let mut best = BaudDiv {
        valid: false,
        cks: 0,
        abcs: false,
        bgdm: false,
        brr: 0,
        err_ppm: u32::MAX,
    };
    if baud == 0 {
        return best;
    }
    let mut mi = 0;
    while mi < BAUD_MODES.len() {
        let (bgdm, abcs, base) = BAUD_MODES[mi];
        let mut cks: u32 = 0;
        while cks < 4 {
            let cpb = base << (2 * cks); // base cycles/bit * 4^cks
            let denom = cpb * baud;
            let bp1 = (pclk + denom / 2) / denom; // rounded (BRR + 1)
            if bp1 >= 1 && bp1 <= 256 {
                let actual = pclk / (cpb * bp1);
                let ppm_u64 = actual.abs_diff(baud) * 1_000_000 / baud;
                let err_ppm = if ppm_u64 > u32::MAX as u64 {
                    u32::MAX
                } else {
                    ppm_u64 as u32
                };
                if !best.valid || err_ppm < best.err_ppm {
                    best = BaudDiv {
                        valid: true,
                        cks: cks as u8,
                        abcs,
                        bgdm,
                        brr: (bp1 - 1) as u8,
                        err_ppm,
                    };
                }
            }
            cks += 1;
        }
        mi += 1;
    }
    best
}

// Golden-vector checks (compile-time; PCLKB = 24 MHz on this board).
const _: () = {
    let d = compute_baud(24_000_000, 115_200);
    assert!(d.valid, "115200 baud unreachable from 24 MHz PCLKB");
    assert!(d.err_ppm < 20_000, "115200 baud error exceeds 2%");
};
const _: () = {
    let d = compute_baud(24_000_000, 9_600);
    assert!(d.valid && d.err_ppm < 5_000, "9600 baud error exceeds 0.5%");
};
const _: () = {
    let d = compute_baud(24_000_000, 57_600);
    assert!(d.valid && d.err_ppm < 20_000, "57600 baud error exceeds 2%");
};

/// Assemble the SMR byte (async mode, CM = 0) from framing config and the
/// resolved CKS. Bit map per R01UM0007 (SCI, SMR): CKS[1:0], MP@2, STOP@3,
/// PM@4, PE@5, CHR@6, CM@7.
const fn smr_byte(cfg: &Config, cks: u8) -> u8 {
    let mut v = cks & 0x3; // CM = 0 (async), MP = 0
    if let DataBits::Seven = cfg.data_bits {
        v |= 1 << 6; // CHR
    }
    match cfg.parity {
        Parity::None => {}
        Parity::Even => v |= 1 << 5,             // PE
        Parity::Odd => v |= (1 << 5) | (1 << 4), // PE | PM
    }
    if let StopBits::Two = cfg.stop_bits {
        v |= 1 << 3; // STOP
    }
    v
}

/// Assemble the SEMR byte. Bit map per R01UM0007 (SCI, SEMR): BRME@2, ABCSE@3,
/// ABCS@4, NFEN@5, BGDM@6, RXDESEL@7. Modulation (BRME) and noise filter
/// (NFEN) stay off; ABCS/BGDM come from the resolved divisor.
const fn semr_byte(div: &BaudDiv) -> u8 {
    let mut v = 0u8;
    if div.abcs {
        v |= 1 << 4;
    }
    if div.bgdm {
        v |= 1 << 6;
    }
    v
}

// =============================================================================
// Per-instance async state (wakers)
// =============================================================================

/// Per-SCI-instance async state: one [`AtomicWaker`] per direction. RX errors
/// ride the RX waker (surfaced in-band, see the module S-C notes). Lives in a
/// `'static` per instance (see the `impl_instance!` expansions).
///
/// [`AtomicWaker`] is the standard lock-free ISR→async driver waker (the same
/// primitive embassy-stm32/-rp use for this seam): `register` from the future's
/// poll, `wake` from the bound ISR. Being `Sync`, it needs no `UnsafeCell` /
/// `unsafe impl Sync` scaffolding of its own.
pub struct SciState {
    rx: AtomicWaker,
    tx: AtomicWaker,
}

impl SciState {
    const fn new() -> Self {
        Self {
            rx: AtomicWaker::new(),
            tx: AtomicWaker::new(),
        }
    }
}

// =============================================================================
// Instance trait + concrete SCI markers
// =============================================================================

mod sealed {
    pub trait Sealed {}
}

/// A concrete SCI channel this driver can drive. Sealed — implemented only for
/// the [`Sci0`]/[`Sci1`]/[`Sci2`]/[`Sci9`] markers below.
///
/// # ICU event numbers
///
/// [`RXI_EVENT`](Instance::RXI_EVENT) / [`TXI_EVENT`](Instance::TXI_EVENT) /
/// [`TEI_EVENT`](Instance::TEI_EVENT) are R01UM0007 ICU event numbers (FSP
/// `ELC_EVENT_SCIn_RXI/TXI/TEI` for the R7FA4M1). They are anchored against the
/// two on-silicon-verified events already in this repo — GPT0 CCMPA = `0x057`
/// (`ra4m1_embassy::time_driver`) and USBFS INT = `0x033`
/// (`bootloader::drivers::usbfs`) — with the SCI block following GPT in the
/// table. **BENCH-VERIFY (parent, via M2.5/Renode or silicon):** these have not
/// been silicon-checked in this repo; they are the FSP contiguous SCI ordering.
pub trait Instance: sealed::Sealed {
    /// The PAC singleton whose ownership proves the caller owns this channel.
    type Pac;
    /// Base of this channel's register block, viewed through the `sci0`
    /// superset layout (see the module docs on the sci0/sci2 layout match).
    const BASE: *const ra4m1::sci0::RegisterBlock;
    /// RXI (receive-data-full) ICU event number.
    const RXI_EVENT: u32;
    /// TXI (transmit-data-empty) ICU event number.
    const TXI_EVENT: u32;
    /// TEI (transmit-end) ICU event number.
    const TEI_EVENT: u32;
    /// MSTPCRB bit that clocks this channel (0 = running). R01UM0007, Low Power
    /// Modes chapter: SCI0 = 31, SCI1 = 30, SCI2 = 29, SCI9 = 22.
    const MSTP_BIT: u8;
    /// This channel's `'static` async state.
    fn state() -> &'static SciState;
}

/// Register-block view for instance `T` (see [`Instance::BASE`]).
#[inline]
fn regs<T: Instance>() -> &'static ra4m1::sci0::RegisterBlock {
    // SAFETY: `T::BASE` is the documented base of an SCI register block; the
    // `sci0` and `sci2` blocks are layout-identical for every register accessed
    // here (module docs). The owning `Uart<T>` holds `T::Pac`, so no other
    // typed access aliases this block. Shared-reference MMIO is the PAC model.
    unsafe { &*T::BASE }
}

macro_rules! impl_instance {
    ($marker:ident, $pac:path, $ptr:path, $state:ident, $mstp:expr, $rxi:expr, $txi:expr, $tei:expr) => {
        #[doc = concat!("Marker for the ", stringify!($marker), " channel.")]
        pub struct $marker;
        impl sealed::Sealed for $marker {}
        static $state: SciState = SciState::new();
        impl Instance for $marker {
            type Pac = $pac;
            const BASE: *const ra4m1::sci0::RegisterBlock =
                <$ptr>::PTR as *const ra4m1::sci0::RegisterBlock;
            const RXI_EVENT: u32 = $rxi;
            const TXI_EVENT: u32 = $txi;
            const TEI_EVENT: u32 = $tei;
            const MSTP_BIT: u8 = $mstp;
            fn state() -> &'static SciState {
                &$state
            }
        }
    };
}

impl_instance!(
    Sci0,
    ra4m1::SCI0,
    ra4m1::SCI0,
    SCI0_STATE,
    31,
    0x8D,
    0x8E,
    0x8F
);
impl_instance!(
    Sci1,
    ra4m1::SCI1,
    ra4m1::SCI1,
    SCI1_STATE,
    30,
    0x92,
    0x93,
    0x94
);
impl_instance!(
    Sci2,
    ra4m1::SCI2,
    ra4m1::SCI2,
    SCI2_STATE,
    29,
    0x97,
    0x98,
    0x99
);
impl_instance!(
    Sci9,
    ra4m1::SCI9,
    ra4m1::SCI9,
    SCI9_STATE,
    22,
    0x9C,
    0x9D,
    0x9E
);

// =============================================================================
// Interrupt handlers (the S-C mask-in-ISR / act-in-future pattern)
// =============================================================================

/// RXI (receive-data-full) handler for instance `T`, bound to `IELn` slot
/// `SLOT`. Masks RIE (stopping the level request from re-entering before the
/// task runs) and wakes the RX waker.
///
/// Bind via [`crate::bind_interrupts`]:
/// `IEL2 => sci::RxInterruptHandler<sci::Sci2, 2>;` (the `IELn` name and `SLOT`
/// must agree — [`crate::interrupt`] macro invariant).
pub struct RxInterruptHandler<T: Instance, const SLOT: usize>(PhantomData<T>);
/// TXI (transmit-data-empty) handler for `T`, bound to slot `SLOT`. Masks TIE,
/// wakes the TX waker.
pub struct TxInterruptHandler<T: Instance, const SLOT: usize>(PhantomData<T>);
/// TEI (transmit-end) handler for `T`, bound to slot `SLOT`. Masks TEIE, wakes
/// the TX waker (used by `flush`).
pub struct TeiInterruptHandler<T: Instance, const SLOT: usize>(PhantomData<T>);

// SAFETY (all three): `on_interrupt` is minimal per rules/embassy.md — one MMIO
// modify to mask this event's enable bit, then a waker wake, then return. The
// generated vector has already cleared `IELSR.IR` for `SLOT`.
unsafe impl<T: Instance, const SLOT: usize> InterruptHandler for RxInterruptHandler<T, SLOT> {
    const SLOT: usize = SLOT;
    fn on_interrupt() {
        regs::<T>().scr().modify(|_, w| w.rie().clear_bit());
        T::state().rx.wake();
    }
}

unsafe impl<T: Instance, const SLOT: usize> InterruptHandler for TxInterruptHandler<T, SLOT> {
    const SLOT: usize = SLOT;
    fn on_interrupt() {
        regs::<T>().scr().modify(|_, w| w.tie().clear_bit());
        T::state().tx.wake();
    }
}

unsafe impl<T: Instance, const SLOT: usize> InterruptHandler for TeiInterruptHandler<T, SLOT> {
    const SLOT: usize = SLOT;
    fn on_interrupt() {
        regs::<T>().scr().modify(|_, w| w.teie().clear_bit());
        T::state().tx.wake();
    }
}

// =============================================================================
// Pin routing
// =============================================================================

/// A TXD/RXD pin to route to the SCI, as a `(port, pin)` coordinate plus the
/// 5-bit PSEL peripheral-select code. PSEL is a board/datasheet fact
/// (R01UM0007, "Selecting Peripheral Functions" — SCI pins) supplied by the
/// caller, mirroring how [`ra4m1_embassy::pfs`] takes a PSEL directly.
#[derive(Clone, Copy, Debug, PartialEq, Eq, defmt::Format)]
pub struct Pin {
    /// Port index 0..=9.
    pub port: u8,
    /// Pin index 0..=15.
    pub pin: u8,
    /// 5-bit PSEL peripheral-function code for the SCI function on this pin.
    pub psel: u8,
}

// =============================================================================
// Uart driver
// =============================================================================

/// A full-duplex UART on SCI channel `T` in asynchronous mode.
///
/// Blocking [`embedded_io`] `Read`/`Write` work immediately after
/// [`Uart::new`]. The async [`embedded_io_async`] `Read`/`Write` additionally
/// require the channel's TXI/RXI/TEI lines to be bound with
/// [`crate::bind_interrupts`] and routed with [`Uart::enable_interrupts`].
///
/// The blocking traits busy-poll the status register; they are for
/// non-executor contexts (e.g. early boot, panic paths). **Never call the
/// blocking API from an Embassy task** — use the async traits there
/// (rules/embassy.md).
pub struct Uart<T: Instance> {
    _sci: T::Pac,
}

impl<T: Instance> Uart<T> {
    /// Configure SCI channel `T` for UART at `config`, routing `txd`/`rxd`.
    ///
    /// Consumes the PAC singleton (`sci`) so the channel is constructed exactly
    /// once. Enables the module clock, muxes the pins, programs the framing +
    /// baud divisor, and enables TX/RX. Interrupts are left masked — call
    /// [`Uart::enable_interrupts`] to use the async traits.
    ///
    /// # Panics
    ///
    /// Panics (init-time) if `config.baud` cannot be reached from PCLKB within
    /// the BRR range — an unachievable baud is a configuration bug, surfaced
    /// loudly at construction (rules/principles.md "fail fast").
    #[must_use]
    pub fn new(
        sci: T::Pac,
        pfs_reg: &ra4m1::PFS,
        pmisc: &ra4m1::PMISC,
        txd: Pin,
        rxd: Pin,
        config: Config,
    ) -> Self {
        let div = compute_baud(Clock::PclkB.hz(), config.baud);
        assert!(div.valid, "SCI baud rate unachievable from PCLKB");

        enable_clock::<T>();

        let sci_regs = regs::<T>();
        // Disable TX/RX before touching SMR/SEMR/BRR (R01UM0007, SCI setup).
        // SAFETY: whole-register write of 0 (all control bits off), the
        // documented pre-config state for this driver-owned channel.
        sci_regs.scr().write(|w| unsafe { w.bits(0) });

        pfs::set_peripheral_function(pfs_reg, pmisc, txd.port, txd.pin, txd.psel);
        pfs::set_peripheral_function(pfs_reg, pmisc, rxd.port, rxd.pin, rxd.psel);

        // SAFETY: whole-register writes of documented field compositions
        // (R01UM0007, SCI: SMR async framing, SEMR base-clock/BGDM, BRR
        // divisor) while TX/RX are disabled.
        sci_regs
            .smr()
            .write(|w| unsafe { w.bits(smr_byte(&config, div.cks)) });
        sci_regs.semr.write(|w| unsafe { w.bits(semr_byte(&div)) });
        sci_regs.brr.write(|w| unsafe { w.bits(div.brr) });

        // R01UM0007 (SCI): wait at least one bit interval after setting BRR
        // before enabling TX/RX. Init-time only (not a task), so a cycle delay
        // is acceptable here.
        cortex_m::asm::delay(Clock::Iclk.hz() / config.baud + 64);

        // Enable transmitter + receiver; CKE = 00 (internal clock, no SCK
        // output); interrupt enables stay 0 until a future arms them.
        sci_regs.scr().write(|w| w.te().set_bit().re().set_bit());

        defmt::debug!(
            "SCI UART up: baud target {}, BRR {}, CKS {}, ABCS {}, BGDM {}, err {} ppm",
            config.baud,
            div.brr,
            div.cks,
            div.abcs,
            div.bgdm,
            div.err_ppm
        );

        Self { _sci: sci }
    }

    /// Route this channel's RXI/TXI/TEI ICU events to the given `IELn` slots at
    /// `priority`, and unmask them. Call once after [`crate::bind_interrupts`]
    /// has provided the strong `IELn` symbols for these slots; each slot must
    /// match the `SLOT` of its bound handler (the [`crate::interrupt`] macro
    /// invariant). Required before the async traits will make progress.
    pub fn enable_interrupts(
        &self,
        rxi_slot: usize,
        txi_slot: usize,
        tei_slot: usize,
        priority: u8,
    ) {
        crate::interrupt::enable(rxi_slot, T::RXI_EVENT, priority);
        crate::interrupt::enable(txi_slot, T::TXI_EVENT, priority);
        crate::interrupt::enable(tei_slot, T::TEI_EVENT, priority);
    }
}

/// Clear instance `T`'s MSTPCRB bit (module-start). Read-modify-write of the
/// shared MSTPCRB, done at init before any concurrency (mirrors the base time
/// driver's `MSTPCRD` start).
fn enable_clock<T: Instance>() {
    // SAFETY: shared-reference MMIO to the MSTP block (PAC access model);
    // MSTPCRB is not PRCR-protected on this part (verified by the base time
    // driver's MSTPCRD start). Whole-register RMW clearing only this channel's
    // bit; the RMW contains no `.await`, so it is atomic under the cooperative
    // single executor even if called from a task, and no ISR touches MSTPCRB.
    let mstp = unsafe { &*ra4m1::MSTP::PTR };
    let cur = mstp.mstpcrb.read().bits();
    mstp.mstpcrb
        .write(|w| unsafe { w.bits(cur & !(1u32 << T::MSTP_BIT)) });
    let _ = mstp.mstpcrb.read();
}

/// Classify any pending RX error in `ssr` bits (ORER/FER/PER), highest severity
/// first. `None` if the receive path is clean.
fn rx_error(ssr: &ra4m1::sci0::ssr::R) -> Option<Error> {
    if ssr.orer().bit() {
        Some(Error::Overrun)
    } else if ssr.fer().bit() {
        Some(Error::Framing)
    } else if ssr.per().bit() {
        Some(Error::Parity)
    } else {
        None
    }
}

/// Clear the RX error flags (write-0-to-clear after a read that saw them set;
/// R01UM0007, SCI SSR). Leaves TDRE/RDRF untouched (writing their read-back
/// value has no effect).
fn clear_rx_errors<T: Instance>() {
    regs::<T>()
        .ssr()
        .modify(|_, w| w.orer().clear_bit().fer().clear_bit().per().clear_bit());
}

// -----------------------------------------------------------------------------
// Blocking embedded-io
// -----------------------------------------------------------------------------

impl<T: Instance> embedded_io::ErrorType for Uart<T> {
    type Error = Error;
}

impl<T: Instance> embedded_io::Read for Uart<T> {
    fn read(&mut self, buf: &mut [u8]) -> Result<usize, Error> {
        if buf.is_empty() {
            return Ok(0);
        }
        let sci = regs::<T>();
        // Busy-wait for the first byte or an error (blocking-trait contract).
        loop {
            let ssr = sci.ssr().read();
            if let Some(e) = rx_error(&ssr) {
                clear_rx_errors::<T>();
                return Err(e);
            }
            if ssr.rdrf().bit() {
                break;
            }
        }
        // Drain what is already available without blocking further.
        let mut n = 0;
        while n < buf.len() {
            let ssr = sci.ssr().read();
            if let Some(e) = rx_error(&ssr) {
                if n > 0 {
                    break; // report the good bytes; the error surfaces next call
                }
                clear_rx_errors::<T>();
                return Err(e);
            }
            if !ssr.rdrf().bit() {
                break;
            }
            buf[n] = sci.rdr.read().bits(); // reading RDR clears RDRF
            n += 1;
        }
        Ok(n)
    }
}

impl<T: Instance> embedded_io::Write for Uart<T> {
    fn write(&mut self, buf: &[u8]) -> Result<usize, Error> {
        if buf.is_empty() {
            return Ok(0);
        }
        let sci = regs::<T>();
        for &b in buf {
            while !sci.ssr().read().tdre().bit() {}
            // SAFETY: 8-bit data write to TDR; clears TDRE (R01UM0007, SCI).
            sci.tdr.write(|w| unsafe { w.bits(b) });
        }
        Ok(buf.len())
    }

    fn flush(&mut self) -> Result<(), Error> {
        let sci = regs::<T>();
        while !sci.ssr().read().tend().bit() {}
        Ok(())
    }
}

// -----------------------------------------------------------------------------
// Async embedded-io-async (the S-C reference pattern)
// -----------------------------------------------------------------------------

impl<T: Instance> embedded_io_async::Read for Uart<T> {
    async fn read(&mut self, buf: &mut [u8]) -> Result<usize, Error> {
        if buf.is_empty() {
            return Ok(0);
        }
        poll_fn(|cx| {
            let sci = regs::<T>();

            // Act if the receive path already has data or an error.
            if let Some(count) = drain_rx::<T>(buf)? {
                return Poll::Ready(Ok(count));
            }

            // Not ready: arm the RXI line and re-check to close the race
            // between the check above and enabling the interrupt.
            T::state().rx.register(cx.waker());
            sci.scr().modify(|_, w| w.rie().set_bit());
            if let Some(count) = drain_rx::<T>(buf)? {
                return Poll::Ready(Ok(count));
            }
            Poll::Pending
        })
        .await
    }
}

impl<T: Instance> embedded_io_async::Write for Uart<T> {
    async fn write(&mut self, buf: &[u8]) -> Result<usize, Error> {
        if buf.is_empty() {
            return Ok(0);
        }
        poll_fn(|cx| {
            let sci = regs::<T>();
            if sci.ssr().read().tdre().bit() {
                // SAFETY: 8-bit data write to TDR; clears TDRE.
                sci.tdr.write(|w| unsafe { w.bits(buf[0]) });
                return Poll::Ready(Ok(1));
            }
            T::state().tx.register(cx.waker());
            sci.scr().modify(|_, w| w.tie().set_bit());
            if sci.ssr().read().tdre().bit() {
                sci.tdr.write(|w| unsafe { w.bits(buf[0]) });
                return Poll::Ready(Ok(1));
            }
            Poll::Pending
        })
        .await
    }

    async fn flush(&mut self) -> Result<(), Error> {
        poll_fn(|cx| {
            let sci = regs::<T>();
            if sci.ssr().read().tend().bit() {
                return Poll::Ready(Ok(()));
            }
            T::state().tx.register(cx.waker());
            sci.scr().modify(|_, w| w.teie().set_bit());
            if sci.ssr().read().tend().bit() {
                return Poll::Ready(Ok(()));
            }
            Poll::Pending
        })
        .await
    }
}

/// Drain currently-available RX bytes into `buf` (non-blocking). Returns
/// `Ok(Some(n))` with `n >= 1` when at least one byte was read, `Ok(None)` when
/// the path is clean but empty, or `Err` on a pending receive error.
fn drain_rx<T: Instance>(buf: &mut [u8]) -> Result<Option<usize>, Error> {
    let sci = regs::<T>();
    let mut n = 0;
    while n < buf.len() {
        let ssr = sci.ssr().read();
        if let Some(e) = rx_error(&ssr) {
            if n > 0 {
                break; // deliver good bytes; error surfaces on the next poll
            }
            clear_rx_errors::<T>();
            return Err(e);
        }
        if !ssr.rdrf().bit() {
            break;
        }
        buf[n] = sci.rdr.read().bits(); // reading RDR clears RDRF
        n += 1;
    }
    Ok(if n > 0 { Some(n) } else { None })
}
