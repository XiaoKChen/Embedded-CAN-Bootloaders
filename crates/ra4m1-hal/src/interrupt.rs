//! Interrupt binding + the central ICU/NVIC slot & priority table (spike S-A,
//! plan §4.7).
//!
//! # The mechanism
//!
//! The base crate ([`ra4m1_embassy::time_driver`]) owns a hand-written
//! 240-slot `__INTERRUPTS` vector table whose 32 real entries reference
//! `extern "C"` symbols `IEL0..IEL31`. Its `device.x` supplies a **weak**
//! `PROVIDE(IELn = DefaultHandler)` for each, so an unbound line falls back to
//! cortex-m-rt's `DefaultHandler`. A **strong** `#[no_mangle] fn IELn` in a
//! downstream crate (this HAL, or an app) *overrides* that weak alias for its
//! slot at link time — no edit to the base crate is needed. This is exactly
//! how the base time driver defines `IEL0` and how the bootloader's USBFS
//! driver defines `IEL1` (both proven on silicon).
//!
//! [`bind_interrupts!`] generates those strong `IELn` symbols for you: each
//! generated vector clears the slot's `IELSR.IR` latch (with a read-back — the
//! phantom-reentry fix learned in `usbfs.rs`/`time_driver.rs`) and then calls
//! your [`InterruptHandler::on_interrupt`], which does the wake and returns.
//! Routing an ICU event to the slot and unmasking the NVIC line is
//! [`enable`]; a driver calls it from its own `init`.
//!
//! # Central slot / priority table (rules/embedded.md: one central scheme)
//!
//! This is the HAL half of the project's single interrupt-priority scheme; the
//! base crate's half is the table in [`ra4m1_embassy::time_driver`]. Any HAL
//! driver that claims a slot records it here.
//!
//! | IELSR slot | NVIC line | Owner | Priority | Notes |
//! |-----------|-----------|-------|----------|-------|
//! | 0  | IEL0 | `ra4m1_embassy::time_driver` (GPT0 CCMPA) | `0xF0` (lowest) | **reserved** — the one time driver. Never claimed here. |
//! | 1  | IEL1 | bootloader `drivers::usbfs` (USBFS INT)   | `0xC0`         | **reserved** in the project table. Not linked into HAL apps, but kept reserved so a future USBFS promotion cannot collide ([`FIRST_APP_SLOT`]). |
//! | 2..=31 | IEL2..IEL31 | HAL drivers (this crate) | see [`DEFAULT_APP_PRIORITY`] | claimable via [`bind_interrupts!`] + [`enable`]. |
//!
//! Priority rule: IEL0 is deliberately the **lowest** urgency (`0xF0`) so
//! every app IRQ preempts millisecond timer housekeeping with no inversion.
//! HAL drivers therefore pick a priority numerically **below** `0xF0` (more
//! urgent). [`DEFAULT_APP_PRIORITY`] is a safe middle default; a
//! latency-critical driver may go higher (smaller byte) and must record why
//! in its module docs.

use ra4m1::Interrupt;

/// IELSR slot 0 — the base crate's GPT320 `embassy-time` alarm (IEL0).
/// Reserved; never bind this from the HAL.
pub const TIME_DRIVER_SLOT: usize = 0;

/// IELSR slot 1 — reserved in the project priority table for the bootloader's
/// USBFS driver (IEL1). Not linked into HAL apps today, but kept reserved so a
/// future promotion of that driver into the HAL cannot collide with an app
/// binding.
pub const USBFS_RESERVED_SLOT: usize = 1;

/// First IELSR slot a HAL driver / app may claim (slots 0 and 1 are reserved,
/// see the table above).
pub const FIRST_APP_SLOT: usize = 2;

/// Highest IELSR slot the RA4M1 ICU implements (IELSR0..=IELSR31).
pub const LAST_SLOT: usize = 31;

/// A safe default NVIC priority byte for HAL peripheral interrupts: level 8 of
/// 15 (4 implemented priority bits, left-aligned). Above the time driver's
/// `0xF0` (so peripheral events preempt timer housekeeping) and below a
/// hypothetical hard-latency line. Drivers may override with a documented
/// reason.
pub const DEFAULT_APP_PRIORITY: u8 = 0x80;

/// Implemented by a type bound to an ICU/NVIC line via [`bind_interrupts!`].
///
/// # Safety
///
/// [`on_interrupt`](InterruptHandler::on_interrupt) runs in interrupt context.
/// It must be minimal (rules/embassy.md): record/clear the peripheral's own
/// status, wake the waiting task, and return — no blocking, no long work. The
/// generated vector clears the slot's `IELSR.IR` latch for you (see
/// [`clear_pending`]); the handler must clear any peripheral-level status flag
/// that would otherwise re-assert the event.
pub unsafe trait InterruptHandler {
    /// The ICU `IELSR` slot (== NVIC line number) this handler is bound to.
    /// Must be in [`FIRST_APP_SLOT`]..=[`LAST_SLOT`]; the generated vector
    /// uses it to clear the correct `IELSR.IR` latch.
    const SLOT: usize;

    /// Called from the bound `IELn` vector, after its `IELSR.IR` latch is
    /// cleared. Do the wake and return.
    fn on_interrupt();
}

/// Clear the latched `IELSR.IR` flag for `slot` and read it back so the write
/// lands before exception return — the phantom-reentry fix (`usbfs.rs`,
/// `time_driver.rs`). Called by the [`bind_interrupts!`]-generated vector; also
/// usable by a hand-written ISR.
#[inline]
pub fn clear_pending(slot: usize) {
    // SAFETY: shared-reference MMIO to the ICU register block — the PAC's own
    // access model. `IELSR[slot]` belongs to the driver that bound this slot,
    // so no other typed access aliases it. `IELSR.IR` is write-0-to-clear.
    let icu = unsafe { &*ra4m1::ICU::PTR };
    icu.ielsr[slot].modify(|_, w| w.ir()._0());
    let _ = icu.ielsr[slot].read();
}

/// Route ICU `event` (an R01UM0007 ICU-event number) to `slot`, set the NVIC
/// priority, and unmask the line. Call once from a driver's `init`, **after**
/// [`bind_interrupts!`] has provided the strong `IELn` symbol for `slot`.
///
/// `IELSR` is written only while the line is masked (R01UM0007, ICU chapter:
/// `IELSR` may change only with the interrupt disabled) — mirrors the base
/// time driver and USBFS bring-up.
///
/// # Panics
///
/// Panics (init-time, via [`slot_to_interrupt`]) if `slot > LAST_SLOT`.
pub fn enable(slot: usize, event: u32, priority: u8) {
    let irq = slot_to_interrupt(slot);
    cortex_m::peripheral::NVIC::mask(irq);
    // SAFETY: shared-reference MMIO to the ICU; whole-register write of the
    // documented event number with IR/DTCE clear, while the line is masked.
    let icu = unsafe { &*ra4m1::ICU::PTR };
    icu.ielsr[slot].write(|w| unsafe { w.bits(event) });
    // SAFETY: priority-set + unmask of the just-configured line. The project's
    // critical sections are PRIMASK-based (`critical-section-single-core`), so
    // NVIC unmasking cannot break them.
    unsafe {
        let mut cp = cortex_m::Peripherals::steal();
        cp.NVIC.set_priority(irq, priority);
        cortex_m::peripheral::NVIC::unmask(irq);
    }
}

/// Map an `IELSR` slot index (0..=31) to its [`ra4m1::Interrupt`] NVIC line.
///
/// # Panics
///
/// Panics if `slot > LAST_SLOT` — an unimplemented ICU line is a programming
/// error, surfaced loudly at init (rules/principles.md "fail fast").
#[must_use]
pub fn slot_to_interrupt(slot: usize) -> Interrupt {
    // The ICU lines are contiguous (IELn = n); a match keeps this total and
    // avoids an `as`-cast into a non-exhaustive enum.
    match slot {
        0 => Interrupt::IEL0,
        1 => Interrupt::IEL1,
        2 => Interrupt::IEL2,
        3 => Interrupt::IEL3,
        4 => Interrupt::IEL4,
        5 => Interrupt::IEL5,
        6 => Interrupt::IEL6,
        7 => Interrupt::IEL7,
        8 => Interrupt::IEL8,
        9 => Interrupt::IEL9,
        10 => Interrupt::IEL10,
        11 => Interrupt::IEL11,
        12 => Interrupt::IEL12,
        13 => Interrupt::IEL13,
        14 => Interrupt::IEL14,
        15 => Interrupt::IEL15,
        16 => Interrupt::IEL16,
        17 => Interrupt::IEL17,
        18 => Interrupt::IEL18,
        19 => Interrupt::IEL19,
        20 => Interrupt::IEL20,
        21 => Interrupt::IEL21,
        22 => Interrupt::IEL22,
        23 => Interrupt::IEL23,
        24 => Interrupt::IEL24,
        25 => Interrupt::IEL25,
        26 => Interrupt::IEL26,
        27 => Interrupt::IEL27,
        28 => Interrupt::IEL28,
        29 => Interrupt::IEL29,
        30 => Interrupt::IEL30,
        31 => Interrupt::IEL31,
        _ => panic!("IELSR slot out of range (0..=31)"),
    }
}

/// Bind ICU/NVIC lines to [`InterruptHandler`] implementors, generating the
/// strong `#[no_mangle] fn IELn` vector for each — overriding the base crate's
/// weak `PROVIDE(IELn = DefaultHandler)` at link time (see the module docs).
///
/// Each generated vector clears its slot's `IELSR.IR` latch ([`clear_pending`])
/// then calls the handler's [`InterruptHandler::on_interrupt`]. The
/// `IELn` name you write must match the handler's [`InterruptHandler::SLOT`];
/// the vector clears `SLOT`'s latch, so a mismatch would clear the wrong slot
/// (kept a documented invariant rather than a macro-parsed number, to stay a
/// declarative one-liner per binding).
///
/// ```ignore
/// use ra4m1_hal::{bind_interrupts, interrupt::InterruptHandler};
///
/// struct SciRx;
/// unsafe impl InterruptHandler for SciRx {
///     const SLOT: usize = 2;              // IEL2
///     fn on_interrupt() { /* wake the SCI RX waker */ }
/// }
///
/// bind_interrupts!(struct Irqs { IEL2 => SciRx; });
/// // ... then in the driver's init: interrupt::enable(2, SCI0_RXI_EVENT, prio);
/// ```
#[macro_export]
macro_rules! bind_interrupts {
    ($(#[$attr:meta])* $vis:vis struct $name:ident {
        $($iel:ident => $handler:ty;)*
    }) => {
        $(#[$attr])*
        $vis struct $name;

        $(
            #[allow(non_snake_case)]
            #[no_mangle]
            unsafe extern "C" fn $iel() {
                use $crate::interrupt::InterruptHandler as _;
                $crate::interrupt::clear_pending(<$handler as $crate::interrupt::InterruptHandler>::SLOT);
                <$handler as $crate::interrupt::InterruptHandler>::on_interrupt();
            }
        )*
    };
}
