//! Custom `embassy-time` driver on **GPT320** (spike S2's answer to
//! docs/plan.md §9 Q3: the PAC confirms a 32-bit GPT channel, so no AGT
//! wrap juggling and no SysTick-at-1-kHz fallback).
//!
//! Registers: R01UM0007 (RA4M1 HW manual), General PWM Timer chapter
//! (GTWP, GTCR, GTUDDTYC, GTPR, GTCNT, GTCCRA, GTST), ICU chapter (IELSR,
//! event-number table), Low Power Modes chapter (MSTPCRD).
//!
//! # Counter & tick rate
//!
//! GPT320 free-runs over its full 32-bit range, clocked from **PCLKD**
//! (R01UM0007, Clock Generation Circuit chapter: PCLKD is the GPT count
//! clock) divided by 16: 48 MHz / 16 = **3 MHz**, so one hardware count is
//! exactly one embassy tick (`embassy-time-driver/tick-hz-3_000_000`,
//! pinned in this crate's Cargo.toml). Const-asserted below against
//! `clocks::PCLKD_HZ`.
//!
//! # Wrap story: 64-bit monotonic ticks from a 32-bit counter
//!
//! The counter wraps every 2^32 / 3 MHz ≈ 23.9 min. `embassy-time` expects
//! a monotonic 64-bit tick space, so the driver extends the count in
//! software: every clock read happens inside a critical section that
//! compares the fresh GTCNT against the previous read and increments a
//! 32-bit wrap counter when the value moved backwards. That detection is
//! sound iff reads are never more than one wrap apart — guaranteed by the
//! alarm policy: GTCCRA is *always* armed, at the earlier of the next timer
//! deadline and [`MAX_SLEEP_TICKS`] (a quarter wrap) ahead, so the alarm
//! ISR reads the clock at least four times per wrap even with no timers
//! queued (a ~6-minute idle wakeup — negligible, and the bootloader is
//! bus/USB powered in every mode that runs the executor, docs/plan.md §4).
//!
//! # Project interrupt priority table (docs/plan.md §4)
//!
//! This is the project's single central priority scheme; any future addition
//! must be recorded here (rules/embedded.md). Two interrupts exist: this
//! driver's alarm (IEL0) and — added by U2.3 wiring USBFS into parked mode —
//! the USBFS controller (IEL1, owned by `drivers::usbfs`).
//!
//! | NVIC line | ICU slot | Event (R01UM0007 event table) | Priority | Owner |
//! |-----------|----------|-------------------------------|----------|-------|
//! | IEL0 (#0) | IELSR0   | GPT0 CCMPA = 0x057 (GTCCRA compare match) | 0xF0 (lowest: level 15 of 15, 4 priority bits) | this driver |
//! | IEL1 (#1) | IELSR1   | USBFS INT = 0x033 (combined USB interrupt) | 0xC0 (level 12 of 15) | `drivers::usbfs` |
//!
//! IEL0's lowest priority is deliberate: alarm deadlines are millisecond-
//! class, so USBFS (0xC0) preempts timer housekeeping with no inversion, and
//! any future latency-critical line still slots in above both.
//!
//! # Lifecycle contract
//!
//! [`init`] is called exactly once, after `clocks::init_hoco_48mhz` and
//! before the executor starts; no `embassy-time` API may run before it
//! (GPT320 is module-stopped until then). It must stay on the *non-jump*
//! path of the boot decision: the app-jump sequence runs pre-executor and
//! never calls `init`, so the app inherits GPT320 module-stopped and IEL0
//! masked — R12's "stop the time-driver counter" extension (docs/plan.md
//! §4) holds by construction. The CAN-session GO exit is a full
//! `NVIC_SystemReset`, which also returns the timer to reset state.

use core::cell::RefCell;
use core::task::Waker;

use critical_section::Mutex;
use embassy_time_driver::Driver;
use embassy_time_queue_utils::Queue;

use crate::clocks;

/// The one NVIC line / ICU slot this driver owns (see the priority table).
pub const TIMER_IRQ: ra4m1::Interrupt = ra4m1::Interrupt::IEL0;
/// `TIMER_IRQ` as an `ICU.IELSR[n]` index.
const TIMER_NVIC_SLOT: usize = 0;
/// NVIC priority byte for [`TIMER_IRQ`]: lowest urgency (level 15 of the
/// RA4M1's 4 implemented priority bits, left-aligned in the byte).
pub const TIMER_IRQ_PRIORITY: u8 = 0xF0;

/// IELSR word selecting the GPT0 CCMPA event (GTCCRA compare match) with
/// IR and DTCE clear. Event number 0x057 per R01UM0007's ICU event table
/// (cross-checked against FSP `ra4m1/bsp_elc.h`, `ELC_EVENT_GPT0_CAPTURE_COMPARE_A`).
const IELSR_GPT0_CCMPA: u32 = 0x057;

/// GPT count-clock prescale (GTCR.TPCS = 0b010 → PCLKD/16), the divisor
/// that makes one count equal one embassy tick.
const GPT_PRESCALE: u32 = 16;
/// Driver tick rate: 3 MHz. Must match the `tick-hz-3_000_000` feature on
/// `embassy-time-driver` (Cargo.toml) — both are const-asserted below.
const DRIVER_TICK_HZ: u32 = clocks::PCLKD_HZ / GPT_PRESCALE;
const _: () = assert!(
    DRIVER_TICK_HZ == 3_000_000,
    "GPT prescale no longer yields 3 MHz"
);
const _: () = assert!(
    embassy_time_driver::TICK_HZ == DRIVER_TICK_HZ as u64,
    "embassy tick-hz feature does not match the GPT320 count rate"
);

/// Farthest ahead the alarm is ever armed, in ticks: a quarter of the
/// 32-bit counter wrap (~357 s at 3 MHz). Bounds the idle-ISR interval so
/// wrap detection (see module docs) always sees at most one wrap between
/// clock reads, with 4x margin.
const MAX_SLEEP_TICKS: u64 = 1 << 30;
/// Minimum arming distance, in ticks (~2.7 µs at 3 MHz): GTCCRA only fires
/// on *equality* with GTCNT, so a target must still be ahead of the counter
/// once the register write lands. Tens of CPU cycles of write latency fit
/// well inside 8 ticks (128 CPU cycles).
const MIN_ARM_TICKS: u64 = 8;

/// Mutable driver state, shared between tasks and the alarm ISR — hence the
/// `critical_section::Mutex` (the ISR preempts thread mode; PRIMASK-based
/// critical sections are the one sound RawMutex flavor here,
/// rules/embassy.md).
struct State {
    /// Timer queue (integrated: nodes live in embassy-executor task headers).
    queue: Queue,
    /// GTCNT at the previous clock read; a smaller fresh read means the
    /// counter wrapped exactly once since (module docs).
    last_count: u32,
    /// Completed 32-bit counter wraps — the high word of the 64-bit tick.
    wraps: u32,
}

struct GptTimeDriver {
    state: Mutex<RefCell<State>>,
}

embassy_time_driver::time_driver_impl!(static DRIVER: GptTimeDriver = GptTimeDriver {
    state: Mutex::new(RefCell::new(State {
        queue: Queue::new(),
        last_count: 0,
        wraps: 0,
    }))
});

/// Low 32 bits of a 64-bit tick count — the compare target GTCCRA can hold.
/// Deliberate truncation: [`GptTimeDriver::arm`] only passes targets less
/// than half a counter wrap ahead, so the low word identifies the match
/// point unambiguously.
const fn low_word(ticks: u64) -> u32 {
    (ticks & u32::MAX as u64) as u32
}

impl GptTimeDriver {
    fn gpt() -> &'static ra4m1::gpt320::RegisterBlock {
        // SAFETY: this driver is the sole owner of GPT320 after `init()`
        // (module lifecycle contract); shared-reference MMIO access is the
        // PAC's own model for a register block.
        unsafe { &*ra4m1::GPT320::PTR }
    }

    /// Read the 64-bit monotonic tick count. Caller holds the critical
    /// section (enforced by `&mut State`, only reachable through the mutex).
    fn now_locked(state: &mut State) -> u64 {
        let count = Self::gpt().gtcnt.read().bits();
        if count < state.last_count {
            state.wraps += 1;
        }
        state.last_count = count;
        (u64::from(state.wraps) << u32::BITS) | u64::from(count)
    }

    /// Arm GTCCRA for `deadline` (64-bit ticks, `u64::MAX` = no timer),
    /// clamped to the sleep horizon and the minimum arming distance.
    fn arm(state: &mut State, deadline: u64) {
        loop {
            let now = Self::now_locked(state);
            let target = deadline.min(now + MAX_SLEEP_TICKS).max(now + MIN_ARM_TICKS);
            // SAFETY: whole-register write of a plain 32-bit compare value
            // (R01UM0007, GPT chapter, GTCCRA; write-enabled via GTWP in
            // `init`). Writing while the counter runs is permitted;
            // buffering is off (GTBER reset state), so it takes effect
            // immediately.
            Self::gpt()
                .gtccra
                .write(|w| unsafe { w.bits(low_word(target)) });
            // Equality-match check: if the counter already passed the target
            // while we wrote (e.g. a debugger stall froze the CPU but not
            // the counter), the match was missed — re-arm farther out.
            if Self::now_locked(state) < target {
                break;
            }
        }
    }

    /// Wake every expired timer and re-arm for the next deadline (or the
    /// sleep horizon when none is queued — the wrap-detection heartbeat).
    fn dispatch(state: &mut State) {
        let now = Self::now_locked(state);
        let next = state.queue.next_expiration(now);
        Self::arm(state, next);
    }

    fn on_interrupt(&self) {
        // SAFETY: IELSR slot 0 is driver-owned (see `init`); this clears the
        // latched IR flag for the event that just fired.
        let icu = unsafe { &*ra4m1::ICU::PTR };
        icu.ielsr[TIMER_NVIC_SLOT].modify(|_, w| w.ir()._0());
        // Flush the IR clear before exception return (FSP's
        // R_BSP_IrqStatusClear does the same read-back) so the NVIC does not
        // re-enter a phantom interrupt.
        let _ = icu.ielsr[TIMER_NVIC_SLOT].read();

        critical_section::with(|cs| Self::dispatch(&mut self.state.borrow_ref_mut(cs)));
    }
}

impl Driver for GptTimeDriver {
    fn now(&self) -> u64 {
        critical_section::with(|cs| Self::now_locked(&mut self.state.borrow_ref_mut(cs)))
    }

    fn schedule_wake(&self, at: u64, waker: &Waker) {
        critical_section::with(|cs| {
            let mut state = self.state.borrow_ref_mut(cs);
            if state.queue.schedule_wake(at, waker) {
                Self::dispatch(&mut state);
            }
        });
    }
}

/// Bring the time base up: module-start GPT320, configure it as a 3 MHz
/// free-running 32-bit counter, link its compare-A event to NVIC line 0,
/// and start counting. Call exactly once, after `clocks::init_hoco_48mhz`
/// and before the executor starts (module lifecycle contract).
pub fn init() {
    // SAFETY: re-acquisition of the PAC singletons for the pieces this
    // driver owns from here on — GPT320 (whole block), ICU.IELSR[0], and
    // MSTPCRD's MSTPD5 bit. Called exactly once, pre-executor; the only
    // earlier steal (`board::init`) touches none of these.
    let p = unsafe { ra4m1::Peripherals::steal() };

    // Module-start the GPT32x group: MSTPD5 = 0 (R01UM0007, Low Power Modes
    // chapter, MSTPCRD — not PRCR-protected). Read back so the module is
    // out of stop before its registers are touched.
    p.MSTP.mstpcrd.modify(|_, w| w.mstpd5()._0());
    let _ = p.MSTP.mstpcrd.read();

    let gpt = &p.GPT320;
    // Write-enable the channel's registers (R01UM0007, GPT chapter, GTWP:
    // key 0xA5 in PRKEY, WP = 0). Reset state is already unlocked; written
    // explicitly so the driver does not depend on nobody locking it first.
    gpt.gtwp.write(|w| w.prkey()._0x_a5().wp()._0());
    // Counter stopped (CST = 0) while mode/prescale change: saw-wave mode
    // (MD = 000), count clock PCLKD/16 (TPCS = 010) → 3 MHz.
    gpt.gtcr.write(|w| w.md()._000().tpcs()._010());
    // Count up, latched immediately via the force bit while stopped
    // (R01UM0007, GTUDDTYC: UD alone is only sampled at overflow).
    gpt.gtuddtyc.write(|w| w.ud()._1().udf()._1());
    gpt.gtuddtyc.write(|w| w.ud()._1().udf()._0());
    // SAFETY: whole-register writes of documented plain values (R01UM0007,
    // GPT chapter): full 32-bit period (counter wraps at 2^32), count from
    // zero, first alarm at the sleep horizon.
    gpt.gtpr.write(|w| unsafe { w.bits(u32::MAX) });
    gpt.gtcnt.write(|w| unsafe { w.bits(0) });
    gpt.gtccra
        .write(|w| unsafe { w.bits(low_word(MAX_SLEEP_TICKS)) });

    // Event link while the NVIC line is still masked (R01UM0007, ICU
    // chapter: IELSR may only change with the corresponding interrupt
    // disabled).
    // SAFETY: whole-register write — IELS = GPT0 CCMPA (0x057), IR and
    // DTCE cleared, exactly the documented field layout.
    p.ICU.ielsr[TIMER_NVIC_SLOT].write(|w| unsafe { w.bits(IELSR_GPT0_CCMPA) });

    // SAFETY: priority-set + unmask of the driver's own, just-configured
    // vector. The project's critical sections are PRIMASK-based
    // (`critical-section-single-core`), so NVIC unmasking cannot break them.
    unsafe {
        let mut cp = cortex_m::Peripherals::steal();
        cp.NVIC.set_priority(TIMER_IRQ, TIMER_IRQ_PRIORITY);
        cortex_m::peripheral::NVIC::unmask(TIMER_IRQ);
    }

    gpt.gtcr.modify(|_, w| w.cst()._1());
    defmt::debug!("time driver up: GPT320 free-running @ 3 MHz, alarm on IEL0 (GPT0 CCMPA)");
}

/// The GPT0 CCMPA alarm handler — NVIC slot 0, the project's only ISR
/// (docs/plan.md §4). Minimal by rule: clear the latch, dispatch, return;
/// all logic lives in the driver, nothing blocks.
#[export_name = "IEL0"]
extern "C" fn gpt_alarm_isr() {
    DRIVER.on_interrupt();
}

/// IRQ slots cortex-m-rt reserves after the 16 exception vectors; filling
/// all of them keeps `.vector_table` at exactly 0x400 bytes, the layout
/// bootloader/memory.x documents and U0.2's OFS placement was verified
/// against.
const CORTEX_M_RT_IRQ_SLOTS: usize = 240;
/// NVIC lines the RA4M1's ICU actually implements (IELSR0..31).
const ICU_NVIC_SLOTS: usize = 32;

mod handlers {
    //! Linker-resolved handler symbols: IEL0 is the strong definition above;
    //! IEL1..IEL31 fall back to `DefaultHandler` via this crate's device.x.
    extern "C" {
        pub fn IEL0();
        pub fn IEL1();
        pub fn IEL2();
        pub fn IEL3();
        pub fn IEL4();
        pub fn IEL5();
        pub fn IEL6();
        pub fn IEL7();
        pub fn IEL8();
        pub fn IEL9();
        pub fn IEL10();
        pub fn IEL11();
        pub fn IEL12();
        pub fn IEL13();
        pub fn IEL14();
        pub fn IEL15();
        pub fn IEL16();
        pub fn IEL17();
        pub fn IEL18();
        pub fn IEL19();
        pub fn IEL20();
        pub fn IEL21();
        pub fn IEL22();
        pub fn IEL23();
        pub fn IEL24();
        pub fn IEL25();
        pub fn IEL26();
        pub fn IEL27();
        pub fn IEL28();
        pub fn IEL29();
        pub fn IEL30();
        pub fn IEL31();
        pub fn DefaultHandler();
    }
}

/// The 32 ICU vectors in NVIC order (IELSR slot n → IELn).
const ICU_IRQ_VECTORS: [unsafe extern "C" fn(); ICU_NVIC_SLOTS] = [
    handlers::IEL0,
    handlers::IEL1,
    handlers::IEL2,
    handlers::IEL3,
    handlers::IEL4,
    handlers::IEL5,
    handlers::IEL6,
    handlers::IEL7,
    handlers::IEL8,
    handlers::IEL9,
    handlers::IEL10,
    handlers::IEL11,
    handlers::IEL12,
    handlers::IEL13,
    handlers::IEL14,
    handlers::IEL15,
    handlers::IEL16,
    handlers::IEL17,
    handlers::IEL18,
    handlers::IEL19,
    handlers::IEL20,
    handlers::IEL21,
    handlers::IEL22,
    handlers::IEL23,
    handlers::IEL24,
    handlers::IEL25,
    handlers::IEL26,
    handlers::IEL27,
    handlers::IEL28,
    handlers::IEL29,
    handlers::IEL30,
    handlers::IEL31,
];

/// Device interrupt vector table, linked by cortex-m-rt's `device` build
/// into `.vector_table` at flash 0x40 (U0.2's report prescribed the
/// hand-written table + device.x instead of the PAC's `rt` feature, which
/// pulls the conflicting cortex-m-rt 0.6 runtime). The 32 real ICU vectors
/// sit inside the same 240-slot span cortex-m-rt's non-`device` build
/// emits, padded with `DefaultHandler`, so the vector-table size — and the
/// `.ofs` placement at 0x400 behind it — is byte-for-byte-layout identical
/// to U0.2's binary.
#[link_section = ".vector_table.interrupts"]
#[no_mangle]
#[used]
pub static __INTERRUPTS: [unsafe extern "C" fn(); CORTEX_M_RT_IRQ_SLOTS] = {
    let mut table: [unsafe extern "C" fn(); CORTEX_M_RT_IRQ_SLOTS] =
        [handlers::DefaultHandler; CORTEX_M_RT_IRQ_SLOTS];
    let mut slot = 0;
    while slot < ICU_NVIC_SLOTS {
        table[slot] = ICU_IRQ_VECTORS[slot];
        slot += 1;
    }
    table
};
