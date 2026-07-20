//! ADC14 (14-bit SAR ADC, S12AD) — blocking + async single/scan reads
//! (M3.4, plan §4.2/§4.3).
//!
//! # What this module implements
//!
//! A **bespoke inherent** API (`embedded-hal` 1.0 has **no** stable ADC trait —
//! the 0.2 trait was dropped, plan §4.3/Q8 — so no trait is implemented):
//!
//! - [`Adc::read`] — blocking one-shot conversion of a single channel
//!   (start → poll `ADCSR.ADST` → read `ADDR[n]`).
//! - [`Adc::scan`] — blocking one scan over a channel bit-mask, filling a
//!   result slice (multi-channel).
//! - [`Adc::read_async`] — awaits the scan-complete interrupt (S12ADI0) via a
//!   waker set from a minimal ISR bound with [`crate::bind_interrupts`] — never
//!   a poll loop (rules/embassy.md). See [`AdcScanInterruptHandler`].
//!
//! Calibration / scaling to millivolts is pure math left to the caller (the ADC
//! returns raw 14-bit right-justified counts, 0..=16383).
//!
//! # Register recipe (R01UM0007, 14-Bit A/D Converter (S12AD) chapter; Low
//! Power Modes chapter for MSTP; ICU chapter for the event link)
//!
//! | Step | Register / bit | Value | Purpose |
//! |---|---|---|---|
//! | Module-start | `MSTP.MSTPCRD.MSTPD16` | 0 | release ADC14 from module-stop |
//! | Resolution | `ADC140.ADCER.ADPRC` | 0b11 | 14-bit accuracy |
//! | Format | `ADC140.ADCER.ADRFMT` | 0 | right-justified result in `ADDR` |
//! | Scan mode | `ADC140.ADCSR.ADCS` | 0b00 | single scan |
//! | Channel(s) | `ADC140.ADANSA0` (+`ADANSA1`) | bit `c` = AN00`c` | select channel(s) |
//! | Start | `ADC140.ADCSR.ADST` | 1 | begin conversion (self-clears on done) |
//! | Result | `ADC140.ADDR[c]` | read | 14-bit count for channel `c` |
//!
//! # Channel range (this phase)
//!
//! Channels **0..=14** (AN000..AN014) are supported — the `ADDR[0..15]` array
//! the PAC types directly, covering every UNO R4 Minima analog header pin. The
//! high-group channels (AN016+, separate `ADDR16..` registers) are not wired up
//! here; [`Adc::read`]/[`Adc::scan`] reject out-of-range channels with
//! [`AdcError::ChannelOutOfRange`]. There is no `AN015` on this device.
//!
//! # Scan-complete interrupt (S12ADI0)
//!
//! In single-scan mode the S12ADI0 event fires unconditionally at scan end (the
//! RA4M1 S12AD has no per-group-A interrupt-enable bit in `ADCSR`; the event is
//! gated only at the ICU). [`Adc::enable_scan_interrupt`] routes ICU event
//! **0x029** (`ELC_EVENT_ADC0_SCAN_END` = 41, cross-checked against FSP
//! `ra4m1/bsp_elc.h` and the base crate's confirmed anchors USBFS=0x033,
//! GPT0=0x057) to a claimed IELSR slot at a chosen NVIC priority.
//!
//! # Waker storage
//!
//! The async path needs a `'static`, `Sync` waker. It uses
//! [`embassy_sync::waitqueue::AtomicWaker`] — the standard lock-free ISR→async
//! driver waker, shared by every async driver in this HAL
//! ([`crate::sci`]/[`crate::spi`]/[`crate::i2c`]). A separate [`AtomicBool`]
//! ([`SCAN_DONE`]) latches scan completion, since the S12AD exposes no
//! re-readable status once `ADCSR.ADST` self-clears at scan end.

use core::future::poll_fn;
use core::sync::atomic::{AtomicBool, Ordering};
use core::task::Poll;

use embassy_sync::waitqueue::AtomicWaker;
use ra4m1::ADC140;

use crate::interrupt::{self, InterruptHandler};

/// Highest ADC channel this phase supports (AN000..AN014 via `ADDR[0..15]`).
pub const MAX_CHANNEL: u8 = 14;

/// ICU event number for the ADC14 group-A scan-end interrupt (S12ADI0):
/// `ELC_EVENT_ADC0_SCAN_END` = 41 (R01UM0007 ICU event table; FSP
/// `ra4m1/bsp_elc.h`).
pub const SCAN_END_EVENT: u32 = 0x029;

/// Upper bound on the blocking poll of `ADCSR.ADST`. A single conversion at
/// 48 MHz PCLKC completes in a few µs; this loop count (~ms of CPU time) is a
/// generous ceiling that turns a mis-configured, never-completing scan into a
/// loud [`AdcError::Timeout`] instead of a hang (rules/principles "fail fast").
const POLL_TIMEOUT: u32 = 100_000;

/// Errors from a blocking or async ADC read.
#[derive(Clone, Copy, Debug, PartialEq, Eq, defmt::Format)]
#[non_exhaustive]
pub enum AdcError {
    /// The requested channel is outside the supported range (0..=[`MAX_CHANNEL`]).
    ChannelOutOfRange,
    /// `ADCSR.ADST` did not clear within [`POLL_TIMEOUT`] — the scan never
    /// completed (blocking path only). Indicates a clock/module mis-config.
    Timeout,
}

/// Set by the bound ISR when a scan completes; cleared by [`Adc::read_async`]
/// before it starts a scan. This flag is the scan's arrival latch (the ADC has
/// no re-readable status once `ADST` self-clears), so it stays alongside the
/// waker.
static SCAN_DONE: AtomicBool = AtomicBool::new(false);
/// Waker for the task awaiting the scan-complete interrupt: [`AtomicWaker`], the
/// standard lock-free ISR→async driver waker (shared with [`crate::sci`] etc.).
static SCAN_WAKER: AtomicWaker = AtomicWaker::new();

/// The RA4M1 module-stop register block, via the PAC singleton pointer.
///
/// `MSTPCRD` is shared across drivers (base time driver `MSTPD5`, this ADC
/// `MSTPD16`, the DAC `MSTPD20`); each clears only its own bit, at init, before
/// the executor starts.
fn mstp() -> &'static ra4m1::mstp::RegisterBlock {
    // SAFETY: shared-reference MMIO to the MSTP block — the PAC's own access
    // model (mirrors `interrupt.rs`'s `&*ICU::PTR`). This driver touches only
    // MSTPCRD.MSTPD16; the read-modify-write runs once at init on a single
    // thread, so it does not race the base crate's or DAC's bring-up.
    unsafe { &*ra4m1::MSTP::PTR }
}

/// Bind this to an IELSR slot with [`crate::bind_interrupts`] to route the ADC
/// scan-complete interrupt (S12ADI0) to the async waker. `SLOT` is the IELSR
/// slot (== NVIC line) the app claims (see [`crate::interrupt`]); it must match
/// the slot passed to [`Adc::enable_scan_interrupt`].
///
/// ```ignore
/// bind_interrupts!(struct Irqs { IEL2 => adc::AdcScanInterruptHandler<2>; });
/// // ...
/// adc.enable_scan_interrupt(2, interrupt::DEFAULT_APP_PRIORITY);
/// ```
pub struct AdcScanInterruptHandler<const SLOT: usize>;

// SAFETY: `on_interrupt` is minimal (rules/embassy.md) — the generated vector
// already cleared the IELSR.IR latch; there is no ADC peripheral-level status
// flag to clear (ADST self-clears at scan end, so the event is one-shot). It
// only records completion and wakes the waiting task.
unsafe impl<const SLOT: usize> InterruptHandler for AdcScanInterruptHandler<SLOT> {
    const SLOT: usize = SLOT;
    fn on_interrupt() {
        SCAN_DONE.store(true, Ordering::Release);
        SCAN_WAKER.wake();
    }
}

/// The RA4M1 14-bit A/D converter, configured for single-scan software-triggered
/// conversions.
///
/// Constructed once from the [`ADC140`] PAC singleton (proving exclusive
/// ownership). The analog input pin for a channel must be configured by the
/// caller for analog use via [`ra4m1_embassy::pfs::set_analog`] (PmnPFS.ASEL = 1
/// with the GPIO input path disconnected); leaving ASEL unset still reads
/// (the analog mux connects at the pad) but ASEL improves isolation/accuracy —
/// `dac.rs` sets it on its output pin the same way.
pub struct Adc {
    adc: ADC140,
}

impl Adc {
    /// Bring up ADC14: release module-stop, select 14-bit right-justified
    /// results and single-scan software-triggered mode. Conversions are started
    /// per-read; no scan runs until [`Adc::read`]/[`Adc::scan`]/[`Adc::read_async`].
    #[must_use]
    pub fn new(adc: ADC140) -> Self {
        // Module-start ADC14 (MSTPD16 = 0), read back so it is clocked before
        // register access (R01UM0007, Low Power Modes chapter).
        mstp().mstpcrd.modify(|_, w| w.mstpd16()._0());
        let _ = mstp().mstpcrd.read();

        // 14-bit accuracy, right-justified result. ADCER may change only while
        // ADST = 0 (true at reset / before any scan).
        adc.adcer
            .modify(|_, w| w.adprc()._11().adrfmt().clear_bit());
        // Single-scan mode, software trigger (TRGE = 0 at reset).
        adc.adcsr.modify(|_, w| w.adcs()._00());

        Self { adc }
    }

    /// Blocking single-channel conversion. Selects `channel`, starts a scan,
    /// polls `ADCSR.ADST` until it self-clears, and returns the 14-bit count
    /// (0..=16383, right-justified).
    ///
    /// # Errors
    ///
    /// [`AdcError::ChannelOutOfRange`] if `channel > MAX_CHANNEL`;
    /// [`AdcError::Timeout`] if the scan does not complete within
    /// [`POLL_TIMEOUT`] poll iterations.
    pub fn read(&mut self, channel: u8) -> Result<u16, AdcError> {
        if channel > MAX_CHANNEL {
            return Err(AdcError::ChannelOutOfRange);
        }
        self.select_channels(1 << channel);
        self.start();
        self.poll_until_done()?;
        Ok(self.result(channel))
    }

    /// Blocking multi-channel scan. Runs one scan over every channel whose bit
    /// is set in `channels` (bit `c` = AN00`c`, `c` in 0..=[`MAX_CHANNEL`]) and
    /// writes each result into `out[c]`. Entries for unselected channels are
    /// left untouched.
    ///
    /// # Errors
    ///
    /// [`AdcError::ChannelOutOfRange`] if `channels` selects a bit above
    /// [`MAX_CHANNEL`]; [`AdcError::Timeout`] on a stuck scan.
    ///
    /// # Panics
    ///
    /// Debug-asserts that `out.len() > MAX_CHANNEL` (room for every result).
    pub fn scan(&mut self, channels: u16, out: &mut [u16]) -> Result<(), AdcError> {
        debug_assert!(out.len() > MAX_CHANNEL as usize);
        let valid_mask: u16 = (1 << (MAX_CHANNEL + 1)) - 1;
        if channels & !valid_mask != 0 {
            return Err(AdcError::ChannelOutOfRange);
        }
        self.select_channels(channels);
        self.start();
        self.poll_until_done()?;
        for channel in 0..=MAX_CHANNEL {
            if channels & (1 << channel) != 0 {
                out[channel as usize] = self.result(channel);
            }
        }
        Ok(())
    }

    /// Route the scan-complete interrupt (S12ADI0, event [`SCAN_END_EVENT`]) to
    /// IELSR `slot` at NVIC `priority`, and unmask it. Call once, **after**
    /// [`crate::bind_interrupts`] has provided the strong `IELn` symbol whose
    /// [`AdcScanInterruptHandler`] `SLOT` equals `slot`. Required before
    /// [`Adc::read_async`].
    pub fn enable_scan_interrupt(&mut self, slot: usize, priority: u8) {
        interrupt::enable(slot, SCAN_END_EVENT, priority);
    }

    /// Async single-channel conversion: selects `channel`, starts a scan, and
    /// awaits the scan-complete interrupt (no busy-wait). Requires
    /// [`Adc::enable_scan_interrupt`] + a bound [`AdcScanInterruptHandler`].
    ///
    /// # Errors
    ///
    /// [`AdcError::ChannelOutOfRange`] if `channel > MAX_CHANNEL`.
    pub async fn read_async(&mut self, channel: u8) -> Result<u16, AdcError> {
        if channel > MAX_CHANNEL {
            return Err(AdcError::ChannelOutOfRange);
        }
        SCAN_DONE.store(false, Ordering::Release);
        self.select_channels(1 << channel);
        self.start();

        poll_fn(|cx| {
            // Register the waker before checking the flag, so a completion that
            // races the poll is never missed.
            SCAN_WAKER.register(cx.waker());
            if SCAN_DONE.load(Ordering::Acquire) {
                Poll::Ready(())
            } else {
                Poll::Pending
            }
        })
        .await;

        Ok(self.result(channel))
    }

    /// Select the scan channel set (`ADANSA0` = channels 0..=15). Whole-register
    /// write of the mask; the PAC types these as per-bit fields but a mask write
    /// is channel-generic (same discipline as `interrupt.rs`'s IELSR writes).
    fn select_channels(&mut self, mask: u16) {
        self.adc.adansa0.write(|w| unsafe { w.bits(mask) });
        self.adc.adansa1.write(|w| unsafe { w.bits(0) });
    }

    /// Start a scan: `ADCSR.ADST = 1`. Self-clears when the scan completes.
    fn start(&mut self) {
        self.adc.adcsr.modify(|_, w| w.adst()._1());
    }

    /// Read the 14-bit right-justified result for `channel` from `ADDR[channel]`.
    /// Caller guarantees `channel <= MAX_CHANNEL`.
    fn result(&self, channel: u8) -> u16 {
        self.adc.addr[channel as usize].read().addr().bits()
    }

    /// Busy-poll `ADCSR.ADST` until the scan completes or [`POLL_TIMEOUT`] is hit.
    fn poll_until_done(&self) -> Result<(), AdcError> {
        for _ in 0..POLL_TIMEOUT {
            if self.adc.adcsr.read().adst().bit_is_clear() {
                return Ok(());
            }
        }
        Err(AdcError::Timeout)
    }
}
