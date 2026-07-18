//! `embassy-usb-driver` implementation for the RA4M1's USBFS full-speed
//! **device** controller (U2.1, docs/plan.md §6 M2). Zero Rust prior art
//! exists for this peripheral (ECO §1/§7); the register layer is a faithful
//! port of TinyUSB's bench-proven RUSB2 device-controller driver
//! (`main:vendor/tinyusb/src/portable/renesas/rusb2/dcd_rusb2.c`), which ran
//! on this exact chip in the C bootloader. Every register/bit name is
//! cross-checked against the `ra4m1` 0.2 PAC's `usbfs` block; facts the
//! TinyUSB source leaves implicit are cited to R01UM0007 (RA4M1 HW manual),
//! USBFS chapter (§28).
//!
//! # Scope (DFU, not a general HAL)
//!
//! Full-speed **device** only. The control endpoint (EP0 = DCP, 64 B) is the
//! critical path — DFU 1.1 is control-transfer-only, so [`ControlPipe`] is
//! implemented in full. One interrupt **IN** endpoint (PIPE6/7) is supported
//! so a minimal composite device enumerates; the other seven pipes and all
//! non-control OUT/bulk/iso transfers are **not** built here — they return
//! [`EndpointAllocError`] loudly and are documented `unimplemented-for-M2`.
//!
//! # Register-recipe map (TinyUSB DCD → this file)
//!
//! | TinyUSB routine | Here | R01UM0007 §28 registers |
//! |---|---|---|
//! | `dcd_init` (FS path) | [`Bus::enable`] | SYSCFG, PHYSLEW, DPUSR0R_FS, DCPMAXP, INTENB0, BRDYENB/BEMPENB |
//! | `dcd_connect`/`disconnect` | [`Bus::enable`]/[`Bus::disable`] | SYSCFG.DPRPU |
//! | `dcd_int_handler` | [`on_interrupt`] | INTSTS0, BRDYSTS, BEMPSTS |
//! | `process_setup_packet` | [`ControlPipe::setup`] | USBREQ/VAL/INDX/LENG, CFIFOCTR, INTSTS0.VALID |
//! | `pipe0_xfer_in`/`process_pipe0_xfer` | [`ControlPipe::data_in`] | CFIFOSEL, CFIFO, CFIFOCTR, DCPCTR |
//! | `pipe0_xfer_out` | [`ControlPipe::data_out`] | CFIFOSEL, CFIFOL, CFIFOCTR, DCPCTR |
//! | ZLP/`CCPL` status | [`ControlPipe::accept`]/`accept_set_address` | DCPCTR.CCPL |
//! | `dcd_edpt_stall` | [`ControlPipe::reject`]/[`Bus::endpoint_set_stalled`] | DCPCTR/PIPExCTR.PID |
//! | `process_bus_reset` | [`Bus::poll`] (on [`Event::Reset`]) | CFIFOCTR, PIPECFG/CTR |
//! | `dcd_remote_wakeup` | [`Bus::remote_wakeup`] | DVSTCTR0.WKUP |
//! | `dcd_edpt_open`/`pipe_xfer_in` | [`EndpointIn`] | PIPESEL/CFG/MAXP, PIPExCTR, D0FIFO |
//!
//! # Write-0-to-clear status registers (gotcha, inverse of the usual W1C)
//!
//! INTSTS0/BRDYSTS/BEMPSTS clear by writing **0** to the bits to clear and
//! **1** to the bits to preserve (R01UM0007 §28; TinyUSB comment: "don't
//! write 0 to already-cleared bits"). The ISR therefore writes
//! `!handled | preserve` as a whole-register raw value — a typed per-bit
//! writer cannot express W0C, so these are the only raw `.bits()` status
//! writes, each documented at the call site.
//!
//! # Power-up ordering (compatible with [`usb_rail`], never duplicated)
//!
//! [`usb_rail::power_up_transceiver_rail`](crate::drivers::usb_rail) must run
//! **before** this driver: it releases USBFS module-stop (MSTPCRB.MSTPB11),
//! sets SYSCFG.SCKE, and turns on the USB LDO (USBMC.VDCEN). Parked mode
//! always runs it before CAN init (SPEC §5.3, R33), so U2.3 gets it for free.
//! This driver re-asserts SCKE (idempotent — it gates USBFS register access)
//! and then performs only the *device-mode* bring-up (DPRPU/DCFM/DRPD/USBE,
//! PHY cross-point, DCPMAXP, interrupt enables, NVIC/ICU). It does **not**
//! touch MSTPCRB or VDCEN — that is the rail's job (docs/plan.md §5.3).
//!
//! # Interrupt: ICU/NVIC slot (docs/plan.md §4 priority table)
//!
//! One ICU event is routed: **USBFS INT = 0x033** (51, the combined USBFS
//! interrupt — VBUS/device-state/control/BRDY/BEMP/resume; FSP
//! `ELC_EVENT_USBFS_INT` for RA4M1) → **NVIC line IEL1 / ICU.IELSR1** at
//! priority **0xC0** (level 12 of 15). IEL0 is the time driver at 0xF0
//! (lowest); USB above it means bus events preempt millisecond timer
//! housekeeping with no priority inversion, and nothing preempts USB. The
//! strong `#[no_mangle] IEL1` below overrides `device.x`'s weak
//! `PROVIDE(IEL1 = DefaultHandler)` for that vector slot without editing
//! `crates/ra4m1-embassy`.
//!
//! **U2.3 must add this row to the central table in
//! `crates/ra4m1-embassy/src/time_driver.rs`:**
//!
//! ```text
//! | IEL1 (#1) | IELSR1 | USBFS INT = 0x033 (combined USB interrupt) | 0xC0 (level 12 of 15) | drivers::usbfs |
//! ```
//!
//! # Waker model
//!
//! The ISR stays minimal (rules/embassy.md): read INTSTS0, W0C-clear the
//! handled bits, record them in [`INTR`] (a lock-free `AtomicU16`), wake the
//! relevant [`WakerCell`], and return — no FIFO copies. Data moves in the
//! async [`ControlPipe`]/[`EndpointIn`] methods per embassy-usb's
//! polling model. `embassy-sync`/`critical-section` are not direct
//! dependencies of the `bootloader` crate, so waker storage uses
//! `cortex_m::interrupt::{free, Mutex}` (the single-core PRIMASK critical
//! section cortex-m provides; consistent with the project's
//! `critical-section-single-core` impl since both mask via PRIMASK).

// U2.3 wires this driver into main's parked mode; nothing constructs the
// `Driver` yet. Drop the allow there. (Same convention as U1.1/U1.2/U2.2.)
#![allow(dead_code)]

use core::cell::Cell;
use core::future::poll_fn;
use core::marker::PhantomData;
use core::ptr;
use core::sync::atomic::{AtomicU16, AtomicU8, Ordering};
use core::task::{Poll, Waker};

use cortex_m::interrupt::Mutex;
use embassy_usb_driver::{
    Bus as BusTrait, ControlPipe as ControlPipeTrait, Direction, Driver as DriverTrait, Endpoint,
    EndpointAddress, EndpointAllocError, EndpointError, EndpointIn as EndpointInTrait,
    EndpointInfo, EndpointOut as EndpointOutTrait, EndpointType, Event, Unsupported,
};

// ---------------------------------------------------------------------------
// ICU / NVIC (see the module header priority table)
// ---------------------------------------------------------------------------

/// The NVIC line / ICU slot this driver owns.
const USB_IRQ: ra4m1::Interrupt = ra4m1::Interrupt::IEL1;
/// `USB_IRQ` as an `ICU.IELSR[n]` index.
const USB_IELSR_SLOT: usize = 1;
/// NVIC priority byte: level 12 of 15 (4 implemented priority bits,
/// left-aligned) — above the time driver's 0xF0, below any future
/// hard-latency line.
const USB_IRQ_PRIORITY: u8 = 0xC0;
/// IELSR value selecting the combined **USBFS INT** event (0x033 = 51,
/// FSP `ELC_EVENT_USBFS_INT` for RA4M1) with IR and DTCE clear (R01UM0007,
/// ICU chapter, IELSR field layout — same encoding the time driver uses for
/// GPT0 CCMPA).
const IELSR_USBFS_INT: u32 = 0x033;

// ---------------------------------------------------------------------------
// Register bit masks — ported verbatim from rusb2_type.h, cross-checked
// against the `ra4m1` PAC (R01UM0007 §28). Whole-register raw writes use
// these; single-bit config uses the PAC's typed field writers.
// ---------------------------------------------------------------------------

// INTSTS0 (Interrupt Status Register 0)
const INTSTS0_VBINT: u16 = 0x8000; // VBUS input change
const INTSTS0_RESM: u16 = 0x4000; // resume
const INTSTS0_SOFR: u16 = 0x2000; // SOF received
const INTSTS0_DVST: u16 = 0x1000; // device-state transition
const INTSTS0_CTRT: u16 = 0x0800; // control-transfer stage transition
const INTSTS0_BEMP: u16 = 0x0400; // buffer empty
const INTSTS0_BRDY: u16 = 0x0100; // buffer ready
const INTSTS0_VBSTS: u16 = 0x0080; // VBUS level (read)
const INTSTS0_DVSQ: u16 = 0x0070; // device-state field
const INTSTS0_VALID: u16 = 0x0008; // USB request (SETUP) received
const INTSTS0_CTSQ: u16 = 0x0007; // control-transfer stage field
/// The status bits [`on_interrupt`] acknowledges (W0C). BRDY/BEMP are
/// acknowledged in their own per-pipe status registers instead.
const INTSTS0_HANDLED: u16 =
    INTSTS0_VBINT | INTSTS0_RESM | INTSTS0_SOFR | INTSTS0_DVST | INTSTS0_CTRT;

// INTSTS0.DVSQ device states (field already masked, values in place)
const DVSQ_DEF: u16 = 0x0010; // default (post bus-reset)
const DVSQ_ADDR: u16 = 0x0020; // addressed (hardware set the address)
const DVSQ_SUSP0: u16 = 0x0040; // suspend variants 0..3

// INTSTS0.CTSQ control-transfer stage
const CTSQ_SETUP_RDATA: u16 = 0x0001; // read-data stage began (SETUP seen)

// CFIFOCTR / D0FIFOCTR
const FIFOCTR_BVAL: u16 = 0x8000; // buffer valid (commit written data)
const FIFOCTR_BCLR: u16 = 0x4000; // buffer clear
const FIFOCTR_FRDY: u16 = 0x2000; // FIFO ready
const FIFOCTR_DTLN: u16 = 0x0FFF; // received byte count

// CFIFOSEL / D0FIFOSEL
const FIFOSEL_ISEL_WRITE: u16 = 0x0020; // FIFO write direction (device TX/IN)
const FIFOSEL_MBW_8BIT: u16 = 0x0000; // access width 8-bit
const FIFOSEL_MBW_16BIT: u16 = 0x0400; // access width 16-bit
const FIFOSEL_CURPIPE: u16 = 0x000F; // current-pipe field

// DCPCTR / PIPExCTR PID responses + control bits
const PID_NAK: u16 = 0x0000;
const PID_BUF: u16 = 0x0001;
const PID_STALL: u16 = 0x0002;
const PID_MASK: u16 = 0x0003;
const CTR_CCPL: u16 = 0x0004; // control-transfer end enable (status stage)
const CTR_SQCLR: u16 = 0x0100; // toggle-sequence clear
const CTR_ACLRM: u16 = 0x0200; // auto-buffer clear
const CTR_BSTS: u16 = 0x8000; // buffer status (read)

// PIPECFG
const PIPECFG_TYPE_BULK: u16 = 0x4000;
const PIPECFG_TYPE_INT: u16 = 0x8000;
const PIPECFG_DBLB: u16 = 0x0200; // double buffer
const PIPECFG_SHTNAK: u16 = 0x0080; // NAK after short packet
const PIPECFG_DIR_IN: u16 = 0x0010; // transfer direction: transmit (IN)

/// EP0 (DCP) max packet size for full-speed control (USB 2.0).
const EP0_MAX_PACKET_SIZE: u16 = 64;

/// Bounded spin count for the microsecond hardware handshakes the RUSB2
/// FIFO/direction switches require (CFIFOSEL ISEL settling, FIFO FRDY,
/// SCKE read-back). TinyUSB busy-waits unbounded on these; a bound keeps
/// the driver from wedging on a peripheral fault (matches
/// `usb_rail`/`can0`'s cycle-counted bounds). ~a few µs of margin at
/// 48 MHz; not a protocol wait, so it is not a no-blocking-rule violation.
const HANDSHAKE_SPINS: u32 = 10_000;

/// Interrupt IN pipes this driver can hand out (PIPE6/PIPE7 — the RUSB2
/// interrupt-capable pipes). Bulk/iso and pipes 1–5/8–9 are not built.
const FIRST_INT_PIPE: u8 = 6;
const LAST_INT_PIPE: u8 = 7;

// ---------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------

/// USBFS driver failure modes surfaced above the `embassy-usb-driver` traits
/// (whose own methods return `EndpointAllocError`/`EndpointError`/`Unsupported`).
#[derive(Debug, Clone, Copy, PartialEq, Eq, defmt::Format)]
pub enum UsbfsError {
    /// A capability deliberately not implemented for Milestone 2 (a non-IN
    /// or non-interrupt endpoint, or pipes outside PIPE6/7). Never silent:
    /// the allocation is rejected and logged.
    UnimplementedForM2,
    /// No interrupt IN pipe (PIPE6/PIPE7) was free.
    OutOfPipes,
}

// ---------------------------------------------------------------------------
// Shared ISR ↔ task state
// ---------------------------------------------------------------------------

/// Pending interrupt-event flags, set by [`on_interrupt`] and consumed by the
/// async trait methods. Lock-free: the RA4M1 (Cortex-M4) has native atomics.
struct Flags(AtomicU16);

// Bus events.
const F_RESET: u16 = 1 << 0;
const F_SUSPEND: u16 = 1 << 1;
const F_RESUME: u16 = 1 << 2;
const F_VBUS_ON: u16 = 1 << 3;
const F_VBUS_OFF: u16 = 1 << 4;
// Control-pipe (EP0/DCP) events.
const F_SETUP: u16 = 1 << 5; // SETUP packet received
const F_EP0_BRDY: u16 = 1 << 6; // DCP OUT data received
const F_EP0_BEMP: u16 = 1 << 7; // DCP IN packet sent
const F_CTRL_STATUS: u16 = 1 << 8; // status-stage ZLP completed
                                   // Interrupt IN pipe events (PIPE6/7 collapsed — only one is ever open here).
const F_EP_IN: u16 = 1 << 9;

impl Flags {
    const fn new() -> Self {
        Self(AtomicU16::new(0))
    }
    fn set(&self, bits: u16) {
        self.0.fetch_or(bits, Ordering::AcqRel);
    }
    /// Atomically clear `bits`, returning whether any were set.
    fn take(&self, bits: u16) -> bool {
        (self.0.fetch_and(!bits, Ordering::AcqRel) & bits) != 0
    }
    /// Read `bits` without clearing (abandon detection in the data stages).
    fn peek(&self, bits: u16) -> bool {
        (self.0.load(Ordering::Acquire) & bits) != 0
    }
}

/// A single [`Waker`] slot guarded by the cortex-m PRIMASK critical section
/// (a minimal `AtomicWaker` — `embassy-sync` is not a direct dependency).
struct WakerCell {
    waker: Mutex<Cell<Option<Waker>>>,
}

impl WakerCell {
    const fn new() -> Self {
        Self {
            waker: Mutex::new(Cell::new(None)),
        }
    }

    /// Store `w` so the next [`Self::wake`] resumes it (idempotent for the
    /// same task's waker).
    fn register(&self, w: &Waker) {
        cortex_m::interrupt::free(|cs| {
            let slot = self.waker.borrow(cs);
            match slot.take() {
                Some(existing) if existing.will_wake(w) => slot.set(Some(existing)),
                _ => slot.set(Some(w.clone())),
            }
        });
    }

    /// Wake and clear the stored waker (called from the ISR). The waker is
    /// taken inside the critical section and woken outside it so the wake
    /// (which may run executor bookkeeping) does not hold PRIMASK.
    fn wake(&self) {
        let w = cortex_m::interrupt::free(|cs| self.waker.borrow(cs).take());
        if let Some(w) = w {
            w.wake();
        }
    }
}

/// Module-global driver state, referenced by the free-function ISR and every
/// async method. There is exactly one USBFS controller, so a single static
/// is the natural owner (mirrors the time driver's `DRIVER`).
struct State {
    bus_waker: WakerCell,
    ep0_waker: WakerCell,
    ep_in_waker: WakerCell,
    flags: Flags,
    /// Bitmask of interrupt IN pipes (bit n = PIPEn) currently enabled by the
    /// host's SET_CONFIGURATION — read by [`EndpointIn::wait_enabled`].
    ep_enabled: AtomicU8,
}

static STATE: State = State {
    bus_waker: WakerCell::new(),
    ep0_waker: WakerCell::new(),
    ep_in_waker: WakerCell::new(),
    flags: Flags::new(),
    ep_enabled: AtomicU8::new(0),
};

/// Borrow the USBFS register block. Access is via the PAC's own shared-`&`
/// MMIO model (the register block is a memory-mapped struct); this driver is
/// the peripheral's sole owner from [`Driver::new`] onward, and the owned
/// `ra4m1::USBFS` token in [`Bus`] enforces the one-driver invariant.
#[inline]
fn regs() -> &'static ra4m1::usbfs::RegisterBlock {
    // SAFETY: USBFS::PTR is the fixed 0x4009_0000 base; a shared reference to
    // the register block aliases only through volatile PAC accessors, which
    // is how svd2rust models all MMIO. Single-core, single owner.
    unsafe { &*ra4m1::USBFS::PTR }
}

// ---------------------------------------------------------------------------
// The ISR (NVIC line IEL1) — minimal: snapshot, W0C-clear, record, wake.
// ---------------------------------------------------------------------------

/// Body of the USBFS interrupt. Port of TinyUSB `dcd_int_handler`, mapped to
/// waker/flag signalling. No FIFO copies here (rules/embassy.md).
fn on_interrupt() {
    let usb = regs();
    let is0 = usb.intsts0.read().bits();

    // Acknowledge the top-level status bits we handle. W0C: 0 clears, 1
    // preserves; force VALID=1 so the SETUP flag survives for `setup()`
    // (TinyUSB: "clear active bits except VALID"). R01UM0007 §28, INTSTS0.
    let handled = is0 & INTSTS0_HANDLED;
    // SAFETY: whole-register raw write of the documented W0C acknowledge
    // value; no typed writer can express write-0-to-clear.
    usb.intsts0
        .write(|w| unsafe { w.bits(!handled | INTSTS0_VALID) });

    let mut flags = 0u16;
    let mut wake_bus = false;
    let mut wake_ep0 = false;

    if is0 & INTSTS0_VBINT != 0 {
        flags |= if is0 & INTSTS0_VBSTS != 0 {
            F_VBUS_ON
        } else {
            F_VBUS_OFF
        };
        wake_bus = true;
    }
    if is0 & INTSTS0_RESM != 0 {
        flags |= F_RESUME;
        wake_bus = true;
    }
    if is0 & INTSTS0_DVST != 0 {
        match is0 & INTSTS0_DVSQ {
            DVSQ_DEF => {
                flags |= F_RESET;
                wake_bus = true;
            }
            // Addressed state: hardware latched USBADDR at the SET_ADDRESS
            // status stage — nothing for software to do.
            DVSQ_ADDR => {}
            // Suspend variants are DVSQ >= SUSP0 with bit4 (default) clear.
            s if s >= DVSQ_SUSP0 => {
                flags |= F_SUSPEND;
                wake_bus = true;
            }
            _ => {}
        }
    }
    if is0 & INTSTS0_CTRT != 0 {
        if is0 & INTSTS0_VALID != 0 {
            // A SETUP packet is waiting in the request registers.
            flags |= F_SETUP;
            wake_ep0 = true;
        } else if is0 & INTSTS0_CTSQ == 0 {
            // Idle stage reached: a status-stage ZLP completed.
            flags |= F_CTRL_STATUS;
            wake_ep0 = true;
        }
    }
    if is0 & INTSTS0_BEMP != 0 {
        let s = usb.bempsts.read().bits();
        // SAFETY: W0C — write 0 to clear every latched BEMP bit (TinyUSB
        // clears all). R01UM0007 §28, BEMPSTS.
        usb.bempsts.write(|w| unsafe { w.bits(0) });
        if s & 0x0001 != 0 {
            flags |= F_EP0_BEMP; // DCP (pipe 0)
            wake_ep0 = true;
        }
    }
    if is0 & INTSTS0_BRDY != 0 {
        let enb = usb.brdyenb.read().bits();
        let s = usb.brdysts.read().bits() & enb;
        // SAFETY: W0C — clear only the fired-and-enabled bits (0), preserve
        // the rest (1). R01UM0007 §28, BRDYSTS.
        usb.brdysts.write(|w| unsafe { w.bits(!s) });
        if s & 0x0001 != 0 {
            flags |= F_EP0_BRDY; // DCP (pipe 0)
            wake_ep0 = true;
        }
        if s & ((1 << FIRST_INT_PIPE) | (1 << LAST_INT_PIPE)) != 0 {
            flags |= F_EP_IN;
        }
    }

    if flags != 0 {
        STATE.flags.set(flags);
    }
    if wake_bus {
        STATE.bus_waker.wake();
    }
    if wake_ep0 {
        STATE.ep0_waker.wake();
    }
    if flags & F_EP_IN != 0 {
        STATE.ep_in_waker.wake();
    }
}

/// USBFS interrupt vector (NVIC line IEL1). Strong `#[no_mangle]` definition
/// overriding `device.x`'s weak `PROVIDE(IEL1 = DefaultHandler)` — pulled
/// into the link by `time_driver::__INTERRUPTS[1]`, which references `IEL1`.
#[no_mangle]
extern "C" fn IEL1() {
    on_interrupt();
    // Flush the IELSR IR clear (done in the async layer / not needed here for
    // the level-driven USBFS event) is unnecessary: unlike GPT compare, the
    // USBFS event line de-asserts once INTSTS0/BRDYSTS/BEMPSTS are cleared
    // above, so there is no phantom re-entry to fence.
}

// ---------------------------------------------------------------------------
// Small helpers shared by the control pipe / endpoints
// ---------------------------------------------------------------------------

/// Spin (bounded) until `cond` holds; returns whether it did. Used only for
/// the RUSB2 FIFO/direction hardware handshakes (see [`HANDSHAKE_SPINS`]).
#[inline]
fn spin_until(mut cond: impl FnMut() -> bool) -> bool {
    for _ in 0..HANDSHAKE_SPINS {
        if cond() {
            return true;
        }
    }
    false
}

/// Raw whole-register write of a `PID`/`CCPL` value to DCPCTR. TinyUSB writes
/// DCPCTR as a whole word; the read-only status bits ignore writes.
#[inline]
fn dcpctr_write(val: u16) {
    // SAFETY: documented whole-register control write (R01UM0007 §28, DCPCTR).
    regs().dcpctr.write(|w| unsafe { w.bits(val) });
}

// ---------------------------------------------------------------------------
// Driver
// ---------------------------------------------------------------------------

/// The RA4M1 USBFS device driver handed to `embassy_usb::Builder`. Constructed
/// with the PAC `USBFS` singleton (same ownership pattern as `can0::Can0`);
/// [`Driver::new`] is cheap — hardware bring-up happens in [`Bus::enable`],
/// which embassy-usb calls from `UsbDevice::run`.
///
/// `usb_rail::power_up_transceiver_rail()` MUST have run first (module header).
pub struct Driver<'d> {
    usb: ra4m1::USBFS,
    /// Bitmask of interrupt IN pipes already handed out.
    in_pipes_used: u8,
    _phantom: PhantomData<&'d ()>,
}

impl<'d> Driver<'d> {
    /// Take ownership of the USBFS peripheral. Does not touch hardware beyond
    /// what `usb_rail` already did; call [`Bus::enable`] (via
    /// `UsbDevice::run`) to bring the device online.
    pub fn new(usb: ra4m1::USBFS) -> Self {
        Self {
            usb,
            in_pipes_used: 0,
            _phantom: PhantomData,
        }
    }

    /// Reserve the next free interrupt IN pipe (PIPE6 then PIPE7).
    fn alloc_int_in_pipe(&mut self) -> Option<u8> {
        for pipe in FIRST_INT_PIPE..=LAST_INT_PIPE {
            let bit = 1u8 << pipe;
            if self.in_pipes_used & bit == 0 {
                self.in_pipes_used |= bit;
                return Some(pipe);
            }
        }
        None
    }
}

impl<'d> DriverTrait<'d> for Driver<'d> {
    type EndpointOut = EndpointOut<'d>;
    type EndpointIn = EndpointIn<'d>;
    type ControlPipe = ControlPipe<'d>;
    type Bus = Bus<'d>;

    fn alloc_endpoint_out(
        &mut self,
        _ep_type: EndpointType,
        _ep_addr: Option<EndpointAddress>,
        _max_packet_size: u16,
        _interval_ms: u8,
    ) -> Result<Self::EndpointOut, EndpointAllocError> {
        // DFU is control-only; no OUT endpoint (beyond EP0) is built for M2.
        defmt::warn!(
            "usbfs: alloc_endpoint_out is {} — no OUT pipes built",
            UsbfsError::UnimplementedForM2
        );
        Err(EndpointAllocError)
    }

    fn alloc_endpoint_in(
        &mut self,
        ep_type: EndpointType,
        ep_addr: Option<EndpointAddress>,
        max_packet_size: u16,
        interval_ms: u8,
    ) -> Result<Self::EndpointIn, EndpointAllocError> {
        if ep_type != EndpointType::Interrupt {
            defmt::warn!(
                "usbfs: alloc_endpoint_in type {} is {} — only interrupt IN is built",
                ep_type,
                UsbfsError::UnimplementedForM2
            );
            return Err(EndpointAllocError);
        }
        let pipe = self.alloc_int_in_pipe().ok_or_else(|| {
            defmt::warn!("usbfs: {}", UsbfsError::OutOfPipes);
            EndpointAllocError
        })?;
        // Pipe number == endpoint number keeps the descriptor and the
        // hardware pipe in lock-step (the host addresses by endpoint number).
        let addr =
            ep_addr.unwrap_or_else(|| EndpointAddress::from_parts(pipe as usize, Direction::In));
        Ok(EndpointIn {
            info: EndpointInfo {
                addr,
                ep_type,
                max_packet_size,
                interval_ms,
            },
            pipe,
            _phantom: PhantomData,
        })
    }

    fn start(self, control_max_packet_size: u16) -> (Self::Bus, Self::ControlPipe) {
        let bus = Bus {
            _usb: self.usb,
            _phantom: PhantomData,
        };
        let cp = ControlPipe {
            max_packet_size: control_max_packet_size,
            _phantom: PhantomData,
        };
        (bus, cp)
    }
}

// ---------------------------------------------------------------------------
// Bus
// ---------------------------------------------------------------------------

/// Whole-bus control: power/enable, event polling, endpoint enable/stall,
/// remote wakeup. Owns the `ra4m1::USBFS` token for the driver's lifetime.
pub struct Bus<'d> {
    _usb: ra4m1::USBFS,
    _phantom: PhantomData<&'d ()>,
}

impl Bus<'_> {
    /// Faithful port of TinyUSB `dcd_init` (FS path) + `dcd_connect`.
    fn hw_enable(&mut self) {
        let usb = regs();

        // SCKE gates USBFS register access; re-assert idempotently (the rail
        // set it) and confirm before touching the rest. R01UM0007 §28 SYSCFG.
        usb.syscfg.modify(|_, w| w.scke()._1());
        spin_until(|| usb.syscfg.read().scke().is_1());

        // Device-controller mode, pull-down off, module enabled.
        usb.syscfg
            .modify(|_, w| w.drpd()._0().dcfm()._0().usbe()._1());

        // Full-speed PHY cross-point + transceiver-output settings the proven
        // FS recipe applies. These registers are beyond the `ra4m1` PAC's
        // USBFS block, so raw MMIO in one named place (rules/embedded.md).
        phy_init_fs();

        // Default control pipe (DCP): 64-byte max packet, device-address
        // select 0. R01UM0007 §28 DCPMAXP (MXPS field).
        // SAFETY: whole-register write; MXPS=64, DEVSEL=0.
        usb.dcpmaxp
            .write(|w| unsafe { w.bits(EP0_MAX_PACKET_SIZE) });

        // Enable the interrupt sources the ISR handles (INTENB0), plus the
        // DCP's BRDY/BEMP so control data/completion interrupts fire.
        // SAFETY: INTSTS0 whole-register clear (W0C: 0 everywhere) before
        // arming, so no stale event fires on enable.
        usb.intsts0.write(|w| unsafe { w.bits(0) });
        usb.intenb0.write(|w| {
            w.vbse()
                ._1()
                .rsme()
                ._1()
                .dvse()
                ._1()
                .ctre()
                ._1()
                .bempe()
                ._1()
                .brdye()
                ._1()
        });
        usb.bempenb.write(|w| w.pipe0bempe()._1());
        usb.brdyenb.write(|w| w.pipe0brdye()._1());

        // Route USBFS INT → IEL1 while the NVIC line is still masked
        // (R01UM0007 ICU chapter: IELSR only changes with the IRQ disabled).
        // SAFETY: whole-register IELSR write of the documented event value.
        let icu = unsafe { &*ra4m1::ICU::PTR };
        icu.ielsr[USB_IELSR_SLOT].write(|w| unsafe { w.bits(IELSR_USBFS_INT) });

        // SAFETY: priority-set + unmask of this driver's own, just-configured
        // vector. Critical sections are PRIMASK-based, so NVIC unmasking
        // cannot break them.
        unsafe {
            let mut cp = cortex_m::Peripherals::steal();
            cp.NVIC.set_priority(USB_IRQ, USB_IRQ_PRIORITY);
            cortex_m::peripheral::NVIC::unmask(USB_IRQ);
        }

        // Connect: assert the D+ pull-up so the host sees us (dcd_connect).
        usb.syscfg.modify(|_, w| w.dprpu()._1());
        defmt::debug!("usbfs: device enabled, D+ pull-up on, USB INT on IEL1");
    }

    /// Light bus-reset housekeeping (port of `process_bus_reset`'s DCP part):
    /// clear the control FIFO. Non-control pipes are re-opened by the stack
    /// via `endpoint_set_enabled`, so nothing more is needed for DFU.
    fn hw_bus_reset(&mut self) {
        // SAFETY: BCLR is a write-only control bit; a whole-register write
        // clears the control FIFO (R01UM0007 §28 CFIFOCTR).
        regs().cfifoctr.write(|w| unsafe { w.bits(FIFOCTR_BCLR) });
        STATE.ep_enabled.store(0, Ordering::Release);
    }
}

impl BusTrait for Bus<'_> {
    async fn enable(&mut self) {
        self.hw_enable();
    }

    async fn disable(&mut self) {
        let usb = regs();
        // Disconnect (drop the pull-up) and disable the module (dcd_disconnect
        // + reverse of dcd_init). Leaves MSTPCRB/VDCEN to the rail owner.
        usb.syscfg.modify(|_, w| w.dprpu()._0().usbe()._0());
        // SAFETY: mask all USBFS interrupt sources before returning.
        usb.intenb0.write(|w| unsafe { w.bits(0) });
        cortex_m::peripheral::NVIC::mask(USB_IRQ);
        defmt::debug!("usbfs: device disabled");
    }

    async fn poll(&mut self) -> Event {
        poll_fn(|cx| {
            STATE.bus_waker.register(cx.waker());
            // Priority order: a reset supersedes everything queued before it.
            if STATE.flags.take(F_RESET) {
                self.hw_bus_reset();
                return Poll::Ready(Event::Reset);
            }
            if STATE.flags.take(F_RESUME) {
                return Poll::Ready(Event::Resume);
            }
            if STATE.flags.take(F_SUSPEND) {
                return Poll::Ready(Event::Suspend);
            }
            if STATE.flags.take(F_VBUS_ON) {
                return Poll::Ready(Event::PowerDetected);
            }
            if STATE.flags.take(F_VBUS_OFF) {
                return Poll::Ready(Event::PowerRemoved);
            }
            Poll::Pending
        })
        .await
    }

    fn endpoint_set_enabled(&mut self, ep_addr: EndpointAddress, enabled: bool) {
        let pipe = ep_addr.index() as u8;
        if !(FIRST_INT_PIPE..=LAST_INT_PIPE).contains(&pipe) || !ep_addr.is_in() {
            // Only the interrupt IN pipes are managed here; EP0/DCP is always
            // active and needs no enable, and no other pipe is built.
            return;
        }
        let usb = regs();
        if enabled {
            // Configure the pipe as an interrupt IN endpoint (port of
            // dcd_edpt_open): select, set MAXP, PIDs to BUF. R01UM0007 §28.
            // SAFETY: documented whole-register PIPESEL/PIPECFG/PIPEMAXP/CTR
            // writes; single owner, IRQ path only reads BRDYSTS for this pipe.
            unsafe {
                usb.pipesel.write(|w| w.bits(u16::from(pipe)));
                usb.pipemaxp.write(|w| w.bits(EP0_MAX_PACKET_SIZE));
                usb.pipecfg
                    .write(|w| w.bits(PIPECFG_TYPE_INT | PIPECFG_DIR_IN | u16::from(pipe)));
                pipe_ctr_write(pipe, CTR_ACLRM | CTR_SQCLR);
                pipe_ctr_write(pipe, 0);
                pipe_ctr_write(pipe, PID_BUF);
            }
            usb.brdyenb
                .modify(|r, w| unsafe { w.bits(r.bits() | (1 << pipe)) });
            STATE.ep_enabled.fetch_or(1 << pipe, Ordering::AcqRel);
            STATE.ep_in_waker.wake();
        } else {
            usb.brdyenb
                .modify(|r, w| unsafe { w.bits(r.bits() & !(1 << pipe)) });
            // SAFETY: PID_NAK stops the pipe (whole-register CTR write).
            unsafe { pipe_ctr_write(pipe, PID_NAK) };
            STATE.ep_enabled.fetch_and(!(1 << pipe), Ordering::AcqRel);
        }
    }

    fn endpoint_set_stalled(&mut self, ep_addr: EndpointAddress, stalled: bool) {
        set_stall(ep_addr, stalled);
    }

    fn endpoint_is_stalled(&mut self, ep_addr: EndpointAddress) -> bool {
        (read_pipe_ctr(ep_addr.index() as u8) & PID_MASK) == PID_STALL
    }

    async fn remote_wakeup(&mut self) -> Result<(), Unsupported> {
        // dcd_remote_wakeup: drive resume signalling. R01UM0007 §28 DVSTCTR0.
        regs().dvstctr0.modify(|_, w| w.wkup()._1());
        Ok(())
    }
}

/// Full-speed PHY cross-point + transceiver settings from the proven FS
/// recipe (TinyUSB `dcd_init`). PHYSLEW (0x4009_00F0) and DPUSR0R_FS
/// (0x4009_0400) sit outside the `ra4m1` PAC's USBFS register block, so this
/// is the driver's only raw absolute-address MMIO (R01UM0007 §28, PHY Cross
/// Point Adjustment Register / Deep-Standby USB Transceiver Control Register;
/// offsets from the R7FA4M1AB CMSIS header).
fn phy_init_fs() {
    const PHYSLEW: *mut u32 = 0x4009_00F0 as *mut u32;
    const DPUSR0R_FS: *mut u32 = 0x4009_0400 as *mut u32;
    /// SLEWR00 | SLEWF00 — the FS cross-point slew value the recipe sets.
    const PHYSLEW_FS: u32 = 0x5;
    /// DPUSR0R_FS.FIXPHY0 (bit 4): clear so the transceiver output is module-
    /// controlled (not fixed).
    const DPUSR0R_FS_FIXPHY0: u32 = 1 << 4;

    // SAFETY: fixed-address 32-bit MMIO to two documented USBFS-region
    // registers the PAC omits; single owner, no aliasing, no ISR contention.
    unsafe {
        ptr::write_volatile(PHYSLEW, PHYSLEW_FS);
        let d = ptr::read_volatile(DPUSR0R_FS) & !DPUSR0R_FS_FIXPHY0;
        ptr::write_volatile(DPUSR0R_FS, d);
    }
}

/// Read a pipe's control register (DCPCTR for pipe 0, PIPExCTR otherwise).
fn read_pipe_ctr(pipe: u8) -> u16 {
    let usb = regs();
    match pipe {
        0 => usb.dcpctr.read().bits(),
        1..=5 => usb.pipectr[usize::from(pipe - 1)].read().bits(),
        6 => usb.pipe6ctr.read().bits(),
        7 => usb.pipe7ctr.read().bits(),
        8 => usb.pipe8ctr.read().bits(),
        _ => usb.pipe9ctr.read().bits(),
    }
}

/// Whole-register write to a pipe's control register.
///
/// # Safety
/// `val` must be a valid PID/CCPL/clear control word (R01UM0007 §28).
unsafe fn pipe_ctr_write(pipe: u8, val: u16) {
    let usb = regs();
    match pipe {
        0 => usb.dcpctr.write(|w| w.bits(val)),
        1..=5 => usb.pipectr[usize::from(pipe - 1)].write(|w| w.bits(val)),
        6 => usb.pipe6ctr.write(|w| w.bits(val)),
        7 => usb.pipe7ctr.write(|w| w.bits(val)),
        8 => usb.pipe8ctr.write(|w| w.bits(val)),
        _ => usb.pipe9ctr.write(|w| w.bits(val)),
    }
}

/// Set/clear STALL on an endpoint (port of dcd_edpt_stall/clear_stall).
fn set_stall(ep_addr: EndpointAddress, stalled: bool) {
    let pipe = ep_addr.index() as u8;
    let cur = read_pipe_ctr(pipe) & PID_MASK;
    // SAFETY: PID transition sequence per R01UM0007 §28 (STALL is reached in
    // two writes; a clear goes via SQCLR then back to BUF).
    unsafe {
        if stalled {
            pipe_ctr_write(pipe, cur | PID_STALL);
            pipe_ctr_write(pipe, PID_STALL);
        } else {
            pipe_ctr_write(pipe, CTR_SQCLR);
            pipe_ctr_write(pipe, PID_BUF);
        }
    }
}

// ---------------------------------------------------------------------------
// Control pipe (EP0 / DCP) — the DFU-critical path
// ---------------------------------------------------------------------------

/// EP0 control pipe over the RUSB2 Default Control Pipe (DCP). embassy-usb
/// drives it one ≤64-byte packet per `data_in`/`data_out` call
/// (embassy-usb 0.6 chunks the data stage into `max_packet_size` pieces).
pub struct ControlPipe<'d> {
    max_packet_size: u16,
    _phantom: PhantomData<&'d ()>,
}

impl ControlPipe<'_> {
    /// Load the DCP IN FIFO with `data` (≤ mps) and hand it to the controller;
    /// on `last`, also arm the status stage (CCPL). Port of `pipe0_xfer_in` +
    /// the write half of `process_pipe0_xfer`.
    fn ep0_send(&self, data: &[u8], last: bool) {
        let usb = regs();
        // Select the control FIFO for writing (IN), 16-bit access. Wait for
        // the direction to settle (bounded — TinyUSB busy-waits).
        // SAFETY: documented CFIFOSEL control write.
        usb.cfifosel
            .write(|w| unsafe { w.bits(FIFOSEL_ISEL_WRITE | FIFOSEL_MBW_16BIT) });
        spin_until(|| usb.cfifosel.read().bits() & FIFOSEL_ISEL_WRITE != 0);

        // 16-bit writes, then a trailing byte for an odd length.
        let mut chunks = data.chunks_exact(2);
        for pair in &mut chunks {
            let word = u16::from(pair[0]) | (u16::from(pair[1]) << 8);
            // SAFETY: FIFO data port; any 16-bit value is valid.
            usb.cfifo().write(|w| unsafe { w.bits(word) });
        }
        if let [tail] = chunks.remainder() {
            // SAFETY: 8-bit FIFO data port (CFIFOL).
            usb.cfifol().write(|w| unsafe { w.bits(*tail) });
        }

        // Commit a short/zero packet with BVAL so the controller transmits it.
        if data.len() < usize::from(self.max_packet_size) {
            // SAFETY: BVAL is a write-only control bit.
            usb.cfifoctr.write(|w| unsafe { w.bits(FIFOCTR_BVAL) });
        }

        // Hand the buffer to the controller; on the last packet also complete
        // the control transfer's status stage (CCPL).
        dcpctr_write(if last { CTR_CCPL | PID_BUF } else { PID_BUF });
    }

    /// Read one received DCP OUT packet into `buf`, returning its length.
    /// Port of `pipe0_xfer_out`.
    fn ep0_recv(&self, buf: &mut [u8]) -> usize {
        let usb = regs();
        let dtln = usize::from(usb.cfifoctr.read().bits() & FIFOCTR_DTLN);
        let len = dtln.min(buf.len());
        // Byte reads from CFIFOL (8-bit access selected in ep0_arm_out).
        for slot in buf.iter_mut().take(len) {
            *slot = usb.cfifol().read().bits();
        }
        // Clear the receive buffer to re-arm for the next OUT packet.
        // SAFETY: BCLR write-only control bit.
        usb.cfifoctr.write(|w| unsafe { w.bits(FIFOCTR_BCLR) });
        len
    }

    /// Select the control FIFO for reading (OUT) and arm the DCP to receive.
    fn ep0_arm_out(&self) {
        let usb = regs();
        // SAFETY: documented CFIFOSEL control write (8-bit read, pipe 0).
        usb.cfifosel.write(|w| unsafe { w.bits(FIFOSEL_MBW_8BIT) });
        spin_until(|| usb.cfifosel.read().bits() & FIFOSEL_ISEL_WRITE == 0);
        dcpctr_write(PID_BUF);
    }
}

impl ControlPipeTrait for ControlPipe<'_> {
    fn max_packet_size(&self) -> usize {
        usize::from(self.max_packet_size)
    }

    async fn setup(&mut self) -> [u8; 8] {
        poll_fn(|cx| {
            STATE.ep0_waker.register(cx.waker());
            if !STATE.flags.take(F_SETUP) {
                return Poll::Pending;
            }
            let usb = regs();
            // Clear any stale control-FIFO content (process_setup_packet).
            // SAFETY: BCLR write-only control bit.
            usb.cfifoctr.write(|w| unsafe { w.bits(FIFOCTR_BCLR) });
            // The 8 setup bytes live in the request registers, little-endian.
            let words = [
                usb.usbreq.read().bits(),
                usb.usbval.read().bits(),
                usb.usbindx.read().bits(),
                usb.usbleng.read().bits(),
            ];
            let mut pkt = [0u8; 8];
            for (i, word) in words.iter().enumerate() {
                pkt[i * 2..i * 2 + 2].copy_from_slice(&word.to_le_bytes());
            }
            // Acknowledge the request: clear VALID (W0C: 0 to VALID, 1 else).
            // SAFETY: documented whole-register W0C write.
            usb.intsts0.write(|w| unsafe { w.bits(!INTSTS0_VALID) });
            // A fresh SETUP voids any half-finished data stage from before.
            STATE.flags.take(F_EP0_BRDY | F_EP0_BEMP | F_CTRL_STATUS);
            Poll::Ready(pkt)
        })
        .await
    }

    async fn data_out(
        &mut self,
        buf: &mut [u8],
        first: bool,
        _last: bool,
    ) -> Result<usize, EndpointError> {
        if first {
            self.ep0_arm_out();
        }
        poll_fn(|cx| {
            STATE.ep0_waker.register(cx.waker());
            // Host abandoned this transfer with a new SETUP: bail immediately
            // (leave F_SETUP for setup()) so the stack restarts.
            if STATE.flags.peek(F_SETUP) {
                return Poll::Ready(Err(EndpointError::Disabled));
            }
            if STATE.flags.take(F_EP0_BRDY) {
                return Poll::Ready(Ok(self.ep0_recv(buf)));
            }
            Poll::Pending
        })
        .await
    }

    async fn data_in(
        &mut self,
        data: &[u8],
        _first: bool,
        last: bool,
    ) -> Result<(), EndpointError> {
        if data.len() > usize::from(self.max_packet_size) {
            return Err(EndpointError::BufferOverflow);
        }
        // Wait for the DCP IN buffer to be free, then load and send it.
        // (First packet: free after setup(); later packets: free after the
        // previous BEMP.)
        spin_until(|| regs().dcpctr.read().bits() & CTR_BSTS != 0);
        self.ep0_send(data, last);

        // Wait until the packet has been transmitted (BEMP) or the host
        // abandons the transfer with a new SETUP.
        poll_fn(|cx| {
            STATE.ep0_waker.register(cx.waker());
            if STATE.flags.peek(F_SETUP) {
                return Poll::Ready(Err(EndpointError::Disabled));
            }
            if STATE.flags.take(F_EP0_BEMP) {
                return Poll::Ready(Ok(()));
            }
            Poll::Pending
        })
        .await
    }

    async fn accept(&mut self) {
        // Complete the control transfer: CCPL drives the status-stage ZLP.
        // Non-blocking — the hardware finishes the status stage itself
        // (matches the C/TinyUSB ZLP path; no device-side timeout, §4).
        dcpctr_write(CTR_CCPL | PID_BUF);
    }

    async fn reject(&mut self) {
        // Protocol stall on EP0 (dcd_edpt_stall). Cleared by the next SETUP.
        let cur = regs().dcpctr.read().bits() & PID_MASK;
        dcpctr_write(cur | PID_STALL);
        dcpctr_write(PID_STALL);
    }

    async fn accept_set_address(&mut self, _addr: u8) {
        // The RUSB2 controller latches USBADDR automatically when the
        // SET_ADDRESS status stage completes (R01UM0007 §28; TinyUSB's
        // dcd_set_address is likewise empty). So we only complete the status
        // stage — `_addr` is handled in hardware.
        self.accept().await;
    }
}

// ---------------------------------------------------------------------------
// Endpoints
// ---------------------------------------------------------------------------

/// Interrupt IN endpoint on PIPE6/PIPE7. Present so a minimal composite
/// device enumerates; DFU itself never opens a non-control endpoint. The
/// transfer path is ported from TinyUSB `pipe_xfer_in` (D0FIFO) and is
/// exercised only when a class first uses it (bench-verify then — module
/// header).
pub struct EndpointIn<'d> {
    info: EndpointInfo,
    pipe: u8,
    _phantom: PhantomData<&'d ()>,
}

impl Endpoint for EndpointIn<'_> {
    fn info(&self) -> &EndpointInfo {
        &self.info
    }

    async fn wait_enabled(&mut self) {
        let bit = 1u8 << self.pipe;
        poll_fn(|cx| {
            STATE.ep_in_waker.register(cx.waker());
            if STATE.ep_enabled.load(Ordering::Acquire) & bit != 0 {
                Poll::Ready(())
            } else {
                Poll::Pending
            }
        })
        .await
    }
}

impl EndpointInTrait for EndpointIn<'_> {
    async fn write(&mut self, buf: &[u8]) -> Result<(), EndpointError> {
        if buf.len() > usize::from(self.info.max_packet_size) {
            return Err(EndpointError::BufferOverflow);
        }
        let usb = regs();
        let num = u16::from(self.pipe);

        // Select D0FIFO for this pipe, 16-bit access, and wait for FRDY
        // (bounded). Keeping the interrupt pipe on D0FIFO leaves CFIFO free
        // for the control pipe (TinyUSB's separation).
        // SAFETY: documented D0FIFOSEL control write.
        usb.d0fifosel
            .write(|w| unsafe { w.bits(num | FIFOSEL_MBW_16BIT) });
        let ready = spin_until(|| {
            let sel = usb.d0fifosel.read().bits();
            (sel & FIFOSEL_CURPIPE) == num && (usb.d0fifoctr.read().bits() & FIFOCTR_FRDY != 0)
        });
        if !ready {
            // SAFETY: deselect the FIFO before bailing.
            usb.d0fifosel.write(|w| unsafe { w.bits(0) });
            return Err(EndpointError::Disabled);
        }

        let mut chunks = buf.chunks_exact(2);
        for pair in &mut chunks {
            let word = u16::from(pair[0]) | (u16::from(pair[1]) << 8);
            // SAFETY: D0FIFO data port.
            usb.d0fifo().write(|w| unsafe { w.bits(word) });
        }
        if let [tail] = chunks.remainder() {
            // SAFETY: 8-bit D0FIFO data port (D0FIFOL).
            usb.d0fifol().write(|w| unsafe { w.bits(*tail) });
        }
        if buf.len() < usize::from(self.info.max_packet_size) {
            // SAFETY: BVAL write-only control bit.
            usb.d0fifoctr.write(|w| unsafe { w.bits(FIFOCTR_BVAL) });
        }
        // Release the FIFO; wait for CURPIPE to clear (TinyUSB does the same).
        // SAFETY: documented D0FIFOSEL control write.
        usb.d0fifosel.write(|w| unsafe { w.bits(0) });
        spin_until(|| usb.d0fifosel.read().bits() & FIFOSEL_CURPIPE == 0);

        // Wait for the packet to be transmitted (BRDY for this pipe).
        poll_fn(|cx| {
            STATE.ep_in_waker.register(cx.waker());
            if STATE.flags.take(F_EP_IN) {
                Poll::Ready(Ok(()))
            } else {
                Poll::Pending
            }
        })
        .await
    }
}

/// OUT endpoint type required by the `Driver` trait. `alloc_endpoint_out`
/// always fails (DFU is control-only; no OUT pipe is built for M2), so this
/// is never constructed and its methods are unreachable — kept concrete only
/// to satisfy the associated type.
pub struct EndpointOut<'d> {
    info: EndpointInfo,
    _phantom: PhantomData<&'d ()>,
}

impl Endpoint for EndpointOut<'_> {
    fn info(&self) -> &EndpointInfo {
        &self.info
    }

    async fn wait_enabled(&mut self) {
        // Unreachable: no EndpointOut is ever allocated (M2 scope).
    }
}

impl EndpointOutTrait for EndpointOut<'_> {
    async fn read(&mut self, _buf: &mut [u8]) -> Result<usize, EndpointError> {
        // Unreachable (see the struct docs); never silent if it somehow runs.
        defmt::error!(
            "usbfs: EndpointOut::read is {}",
            UsbfsError::UnimplementedForM2
        );
        Err(EndpointError::Disabled)
    }
}
