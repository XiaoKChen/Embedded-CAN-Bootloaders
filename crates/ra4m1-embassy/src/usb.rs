//! `embassy-usb-driver` implementation for the RA4M1 USBFS full-speed device
//! controller — the shared chip-level USB device driver.
//!
//! # Scope & the IEL1 binding
//!
//! Full-speed device with EP0 control plus bulk IN/OUT and one interrupt IN
//! pipe — enough for USB-CDC (serial over USB). The `bootloader` keeps its own
//! DFU-only `drivers::usbfs` for now (silicon-proven); this module is the
//! general driver the `demo-app` uses and the eventual home the bootloader can
//! rebase onto.
//!
//! This module deliberately does **not** define the `IEL1` interrupt vector
//! (`#[no_mangle]`): `ra4m1-embassy` is linked by both binaries and the
//! bootloader defines its own `IEL1`, so a strong symbol here would collide.
//! Instead it exposes [`on_interrupt`]; each binary binds the vector, e.g.
//! `#[no_mangle] extern "C" fn IEL1() { ra4m1_embassy::usb::on_interrupt() }`.
//! [`power_up_rail`] must run before [`Driver::new`]/`Bus::enable`.
//!
//! # Endpoint / pipe / FIFO map (fixed for CDC's three endpoints)
//!
//! `CdcAcmClass::new` allocates, in order: interrupt-IN (comm, 8 B), bulk-OUT
//! (read, 64 B), bulk-IN (write, 64 B). This driver pins them to fixed RUSB2
//! pipes and FIFO channels:
//!
//! | Endpoint            | Pipe    | FIFO   | Completion IRQ |
//! |---------------------|---------|--------|----------------|
//! | EP0 control (DCP)   | PIPE0   | CFIFO  | BRDY/BEMP/CTRT |
//! | comm interrupt IN   | PIPE6   | D0FIFO | (never written)|
//! | bulk OUT (host→dev) | PIPE1   | D1FIFO | BRDY           |
//! | bulk IN  (dev→host) | PIPE2   | D0FIFO | BEMP           |
//!
//! IN transfers complete on **BEMP** (buffer emptied = packet sent) and OUT
//! on **BRDY** (buffer ready = packet received) — the same split the DCP uses
//! (`ControlPipe::data_in` waits BEMP, `data_out` waits BRDY). D0FIFO is
//! muxed between the comm and bulk-IN pipes by selecting CURPIPE per transfer;
//! safe because everything runs on one cooperative thread-mode executor.
//!
//! The register recipe (SYSCFG/PHY/DCPMAXP/FIFO/PIPECFG) is the RUSB2 device
//! recipe; see `bootloader::drivers::usbfs` for the R01UM0007 §28 citations.

use core::cell::Cell;
use core::future::poll_fn;
use core::marker::PhantomData;
use core::ptr;
use core::sync::atomic::{AtomicBool, AtomicU16, Ordering};
use core::task::{Poll, Waker};

use crate::clocks;
use cortex_m::interrupt::Mutex;
use embassy_usb_driver::{
    Bus as BusTrait, ControlPipe as ControlPipeTrait, Direction, Driver as DriverTrait, Endpoint,
    EndpointAddress, EndpointAllocError, EndpointError, EndpointIn as EndpointInTrait,
    EndpointInfo, EndpointOut as EndpointOutTrait, EndpointType, Event, Unsupported,
};

// --- ICU / NVIC (mirrors bootloader::drivers::usbfs) ------------------------

const USB_IRQ: ra4m1::Interrupt = ra4m1::Interrupt::IEL1;
const USB_IELSR_SLOT: usize = 1;
const USB_IRQ_PRIORITY: u8 = 0xC0;
const IELSR_USBFS_INT: u32 = 0x033;

// --- Fixed pipe assignment for CDC's three endpoints ------------------------

/// Interrupt IN (CDC comm/notification) pipe.
const COMM_INT_PIPE: u8 = 6;
/// Bulk OUT (host→device) pipe — first bulk allocation (`read_ep`).
const BULK_OUT_PIPE: u8 = 1;
/// Bulk IN (device→host) pipe — second bulk allocation (`write_ep`).
const BULK_IN_PIPE: u8 = 2;
/// Interrupt-IN max packet size CDC declares for the comm endpoint.
const COMM_MAX_PACKET: u16 = 8;
/// Full-speed bulk max packet size.
const BULK_MAX_PACKET: u16 = 64;

// --- Register bit masks (subset of usbfs.rs, same values) -------------------

const INTSTS0_VBINT: u16 = 0x8000;
const INTSTS0_RESM: u16 = 0x4000;
const INTSTS0_SOFR: u16 = 0x2000;
const INTSTS0_DVST: u16 = 0x1000;
const INTSTS0_CTRT: u16 = 0x0800;
const INTSTS0_BEMP: u16 = 0x0400;
const INTSTS0_BRDY: u16 = 0x0100;
const INTSTS0_VBSTS: u16 = 0x0080;
const INTSTS0_DVSQ: u16 = 0x0070;
const INTSTS0_VALID: u16 = 0x0008;
const INTSTS0_CTSQ: u16 = 0x0007;
const INTSTS0_HANDLED: u16 =
    INTSTS0_VBINT | INTSTS0_RESM | INTSTS0_SOFR | INTSTS0_DVST | INTSTS0_CTRT;

const DVSQ_DEF: u16 = 0x0010;
const DVSQ_ADDR: u16 = 0x0020;
const DVSQ_SUSP0: u16 = 0x0040;

const FIFOCTR_BVAL: u16 = 0x8000;
const FIFOCTR_BCLR: u16 = 0x4000;
const FIFOCTR_FRDY: u16 = 0x2000;
const FIFOCTR_DTLN: u16 = 0x0FFF;

const FIFOSEL_ISEL_WRITE: u16 = 0x0020;
const FIFOSEL_MBW_8BIT: u16 = 0x0000;
const FIFOSEL_MBW_16BIT: u16 = 0x0400;
const FIFOSEL_CURPIPE: u16 = 0x000F;

const PID_NAK: u16 = 0x0000;
const PID_BUF: u16 = 0x0001;
const PID_STALL: u16 = 0x0002;
const PID_MASK: u16 = 0x0003;
const CTR_CCPL: u16 = 0x0004;
const CTR_SQCLR: u16 = 0x0100;
const CTR_ACLRM: u16 = 0x0200;
const CTR_BSTS: u16 = 0x8000;

const PIPECFG_TYPE_BULK: u16 = 0x4000;
const PIPECFG_TYPE_INT: u16 = 0x8000;
const PIPECFG_SHTNAK: u16 = 0x0080;
const PIPECFG_DIR_IN: u16 = 0x0010;

const EP0_MAX_PACKET_SIZE: u16 = 64;
const HANDSHAKE_SPINS: u32 = 10_000;

// --- Shared ISR <-> task state ----------------------------------------------

struct Flags(AtomicU16);

const F_RESET: u16 = 1 << 0;
const F_SUSPEND: u16 = 1 << 1;
const F_RESUME: u16 = 1 << 2;
const F_VBUS_ON: u16 = 1 << 3;
const F_VBUS_OFF: u16 = 1 << 4;
const F_SETUP: u16 = 1 << 5;
const F_EP0_BRDY: u16 = 1 << 6;
const F_EP0_BEMP: u16 = 1 << 7;
const F_CTRL_STATUS: u16 = 1 << 8;
/// Bulk IN packet transmitted (BEMP on [`BULK_IN_PIPE`]).
const F_BULK_IN: u16 = 1 << 9;
/// Bulk OUT packet received (BRDY on [`BULK_OUT_PIPE`]).
const F_BULK_OUT: u16 = 1 << 10;

impl Flags {
    const fn new() -> Self {
        Self(AtomicU16::new(0))
    }
    fn set(&self, bits: u16) {
        self.0.fetch_or(bits, Ordering::AcqRel);
    }
    fn take(&self, bits: u16) -> bool {
        (self.0.fetch_and(!bits, Ordering::AcqRel) & bits) != 0
    }
    fn peek(&self, bits: u16) -> bool {
        (self.0.load(Ordering::Acquire) & bits) != 0
    }
}

struct WakerCell {
    waker: Mutex<Cell<Option<Waker>>>,
}

impl WakerCell {
    const fn new() -> Self {
        Self {
            waker: Mutex::new(Cell::new(None)),
        }
    }
    fn register(&self, w: &Waker) {
        cortex_m::interrupt::free(|cs| {
            let slot = self.waker.borrow(cs);
            match slot.take() {
                Some(existing) if existing.will_wake(w) => slot.set(Some(existing)),
                _ => slot.set(Some(w.clone())),
            }
        });
    }
    fn wake(&self) {
        let w = cortex_m::interrupt::free(|cs| self.waker.borrow(cs).take());
        if let Some(w) = w {
            w.wake();
        }
    }
}

struct State {
    bus_waker: WakerCell,
    ep0_waker: WakerCell,
    /// Wakes the bulk IN writer and OUT reader and the enable waiters.
    ep_waker: WakerCell,
    flags: Flags,
    /// Bitmask of pipes the host has enabled via SET_CONFIGURATION.
    ep_enabled: AtomicU16,
    power_reported: AtomicBool,
}

static STATE: State = State {
    bus_waker: WakerCell::new(),
    ep0_waker: WakerCell::new(),
    ep_waker: WakerCell::new(),
    flags: Flags::new(),
    ep_enabled: AtomicU16::new(0),
    power_reported: AtomicBool::new(false),
};

#[inline]
fn regs() -> &'static ra4m1::usbfs::RegisterBlock {
    // SAFETY: USBFS::PTR is the fixed 0x4009_0000 base; svd2rust models all
    // MMIO through volatile accessors on a shared reference. Single-core,
    // single owner (the `ra4m1::USBFS` token in `Bus`).
    unsafe { &*ra4m1::USBFS::PTR }
}

// --- USB rail + clock power-up (ported from bootloader::drivers::usb_rail) ---

/// SCKE read-back poll bound (C parity).
const SCKE_READBACK_POLLS: u32 = 1000;
/// Regulator soak before first USB access (engineering margin).
const RAIL_SOAK_MS: u32 = 5;
const RAIL_SOAK_CYCLES: u32 = clocks::ICLK_HZ / 1_000 * RAIL_SOAK_MS;

/// Power the USB transceiver rail and point UCLK at HOCO 48 MHz. MUST run
/// before [`Driver::new`]/`Bus::enable`. Blocking (~5 ms) — pre-executor, a
/// one-shot bring-up delay, exactly as the bootloader's rail power-up.
pub fn power_up_rail() {
    // SAFETY: transient PAC-singleton re-acquisition for the module-stop +
    // USBFS + clock bring-up, before any executor/ISR touches these.
    let p = unsafe { ra4m1::Peripherals::steal() };

    // UCLK source before the module leaves module-stop (USBCKCR resets to the
    // disabled PLL on this HOCO-only plan) — see usb_rail.rs / the 2026-07-17
    // bring-up: without this the USBFS operational domain faults.
    clocks::init_usb_clock_48mhz(&p.SYSTEM);

    p.MSTP.mstpcrb.modify(|_, w| w.mstpb11()._0());
    let _ = p.MSTP.mstpcrb.read();

    p.USBFS.syscfg.modify(|_, w| w.scke()._1());
    for _ in 0..SCKE_READBACK_POLLS {
        if p.USBFS.syscfg.read().scke().is_1() {
            break;
        }
    }

    p.USBFS.usbmc.modify(|_, w| w.vdcen()._1());
    cortex_m::asm::delay(RAIL_SOAK_CYCLES);
}

// --- ISR --------------------------------------------------------------------

/// USBFS interrupt service routine. The consuming binary binds this to the
/// `IEL1` vector: `#[no_mangle] extern "C" fn IEL1() { ra4m1_embassy::usb::on_interrupt() }`.
/// Minimal: clear the ICU request, snapshot INTSTS0, W0C-clear, record flags,
/// wake — no FIFO copies (data moves in the async trait methods).
pub fn on_interrupt() {
    // Clear ICU IELSR IR at ENTRY (RA/FSP ordering) — clearing at exit drops
    // events that arrive mid-handler on the high-rate USB combined line (root
    // cause of the 2026-07-17 USB delivery wedge).
    // SAFETY: IELSR slot 1 is this driver's own (configured in hw_enable).
    let icu = unsafe { &*ra4m1::ICU::PTR };
    icu.ielsr[USB_IELSR_SLOT].modify(|_, w| w.ir()._0());
    let _ = icu.ielsr[USB_IELSR_SLOT].read();

    let usb = regs();
    let is0 = usb.intsts0.read().bits();

    let handled = is0 & INTSTS0_HANDLED;
    // SAFETY: documented W0C acknowledge (0 clears, 1 preserves); force
    // VALID=1 so a pending SETUP survives for `setup()`.
    usb.intsts0
        .write(|w| unsafe { w.bits(!handled | INTSTS0_VALID) });

    let mut flags = 0u16;
    let mut wake_bus = false;
    let mut wake_ep0 = false;
    let mut wake_ep = false;

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
        // SAFETY: disarm the SOF suspend-exit probe (see usbfs.rs RESM arm).
        usb.intenb0.modify(|_, w| w.sofe()._0());
    }
    if is0 & INTSTS0_SOFR != 0 && usb.intenb0.read().sofe().is_1() {
        // SAFETY: one SOF exits suspend; disarm the probe.
        usb.intenb0.modify(|_, w| w.sofe()._0());
    }
    if is0 & INTSTS0_DVST != 0 {
        match is0 & INTSTS0_DVSQ {
            DVSQ_DEF => {
                flags |= F_RESET;
                wake_bus = true;
            }
            DVSQ_ADDR => {}
            s if s >= DVSQ_SUSP0 => {
                flags |= F_SUSPEND;
                wake_bus = true;
                // SAFETY: arm SOF-based suspend-exit (see usbfs.rs SUSPEND arm).
                usb.intenb0.modify(|_, w| w.sofe()._1());
            }
            _ => {}
        }
    }
    if is0 & INTSTS0_CTRT != 0 {
        if is0 & INTSTS0_VALID != 0 {
            flags |= F_SETUP;
            wake_ep0 = true;
        } else if is0 & INTSTS0_CTSQ == 0 {
            flags |= F_CTRL_STATUS;
            wake_ep0 = true;
        }
    }
    if is0 & INTSTS0_BEMP != 0 {
        let enb = usb.bempenb.read().bits();
        let s = usb.bempsts.read().bits() & enb;
        // SAFETY: W0C — clear the fired-and-enabled bits, preserve the rest.
        usb.bempsts.write(|w| unsafe { w.bits(!s) });
        if s & (1 << 0) != 0 {
            flags |= F_EP0_BEMP;
            wake_ep0 = true;
        }
        if s & (1 << BULK_IN_PIPE) != 0 {
            flags |= F_BULK_IN;
            wake_ep = true;
        }
    }
    if is0 & INTSTS0_BRDY != 0 {
        let enb = usb.brdyenb.read().bits();
        let s = usb.brdysts.read().bits() & enb;
        // SAFETY: W0C — clear only the fired-and-enabled bits.
        usb.brdysts.write(|w| unsafe { w.bits(!s) });
        if s & (1 << 0) != 0 {
            flags |= F_EP0_BRDY;
            wake_ep0 = true;
        }
        if s & (1 << BULK_OUT_PIPE) != 0 {
            flags |= F_BULK_OUT;
            wake_ep = true;
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
    if wake_ep {
        STATE.ep_waker.wake();
    }
}

// --- Small helpers ----------------------------------------------------------

#[inline]
fn spin_until(mut cond: impl FnMut() -> bool) -> bool {
    for _ in 0..HANDSHAKE_SPINS {
        if cond() {
            return true;
        }
    }
    false
}

#[inline]
fn dcpctr_write(val: u16) {
    // SAFETY: documented whole-register control write (R01UM0007 §28 DCPCTR).
    regs().dcpctr.write(|w| unsafe { w.bits(val) });
}

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

fn set_stall(ep_addr: EndpointAddress, stalled: bool) {
    let pipe = ep_addr.index() as u8;
    let cur = read_pipe_ctr(pipe) & PID_MASK;
    // SAFETY: PID transition per R01UM0007 §28 (STALL in two writes; clear via
    // SQCLR then BUF).
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

// --- Driver -----------------------------------------------------------------

/// The RA4M1 USBFS device driver handed to `embassy_usb::Builder`, CDC-shaped.
/// [`power_up_rail`] MUST have run first.
pub struct Driver<'d> {
    usb: ra4m1::USBFS,
    /// Next free bulk pipe (PIPE1, PIPE2, ...).
    next_bulk_pipe: u8,
    _phantom: PhantomData<&'d ()>,
}

impl<'d> Driver<'d> {
    /// Take ownership of the USBFS peripheral (no hardware touch beyond what
    /// [`power_up_rail`] did; bring-up is in `Bus::enable`).
    pub fn new(usb: ra4m1::USBFS) -> Self {
        Self {
            usb,
            next_bulk_pipe: BULK_OUT_PIPE,
            _phantom: PhantomData,
        }
    }

    fn alloc_bulk_pipe(&mut self) -> Option<u8> {
        if self.next_bulk_pipe <= 5 {
            let p = self.next_bulk_pipe;
            self.next_bulk_pipe += 1;
            Some(p)
        } else {
            None
        }
    }
}

impl<'d> DriverTrait<'d> for Driver<'d> {
    type EndpointOut = EndpointOut<'d>;
    type EndpointIn = EndpointIn<'d>;
    type ControlPipe = ControlPipe<'d>;
    type Bus = Bus<'d>;

    fn alloc_endpoint_out(
        &mut self,
        ep_type: EndpointType,
        ep_addr: Option<EndpointAddress>,
        max_packet_size: u16,
        interval_ms: u8,
    ) -> Result<Self::EndpointOut, EndpointAllocError> {
        if ep_type != EndpointType::Bulk {
            defmt::warn!("usb-cdc: only bulk OUT is built");
            return Err(EndpointAllocError);
        }
        let pipe = self.alloc_bulk_pipe().ok_or(EndpointAllocError)?;
        let addr =
            ep_addr.unwrap_or_else(|| EndpointAddress::from_parts(pipe as usize, Direction::Out));
        Ok(EndpointOut {
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

    fn alloc_endpoint_in(
        &mut self,
        ep_type: EndpointType,
        ep_addr: Option<EndpointAddress>,
        max_packet_size: u16,
        interval_ms: u8,
    ) -> Result<Self::EndpointIn, EndpointAllocError> {
        let pipe = match ep_type {
            EndpointType::Interrupt => COMM_INT_PIPE,
            EndpointType::Bulk => self.alloc_bulk_pipe().ok_or(EndpointAllocError)?,
            _ => {
                defmt::warn!("usb-cdc: only interrupt/bulk IN are built");
                return Err(EndpointAllocError);
            }
        };
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

// --- Bus --------------------------------------------------------------------

pub struct Bus<'d> {
    _usb: ra4m1::USBFS,
    _phantom: PhantomData<&'d ()>,
}

impl Bus<'_> {
    fn hw_enable(&mut self) {
        let usb = regs();

        usb.syscfg.modify(|_, w| w.scke()._1());
        spin_until(|| usb.syscfg.read().scke().is_1());
        usb.syscfg
            .modify(|_, w| w.drpd()._0().dcfm()._0().usbe()._1());
        phy_init_fs();

        // SAFETY: DCPMAXP MXPS=64, DEVSEL=0.
        usb.dcpmaxp
            .write(|w| unsafe { w.bits(EP0_MAX_PACKET_SIZE) });

        // SAFETY: clear INTSTS0 (W0C: 0 everywhere) before arming.
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

        // SAFETY: IELSR write of the documented USBFS INT event, IRQ masked.
        let icu = unsafe { &*ra4m1::ICU::PTR };
        icu.ielsr[USB_IELSR_SLOT].write(|w| unsafe { w.bits(IELSR_USBFS_INT) });

        // SAFETY: priority-set + unmask of this driver's own vector.
        unsafe {
            let mut cp = cortex_m::Peripherals::steal();
            cp.NVIC.set_priority(USB_IRQ, USB_IRQ_PRIORITY);
            cortex_m::peripheral::NVIC::unmask(USB_IRQ);
        }

        usb.syscfg.modify(|_, w| w.dprpu()._1());
        defmt::debug!("usb-cdc: device enabled, D+ pull-up on, USB INT on IEL1");
    }

    fn hw_bus_reset(&mut self) {
        // SAFETY: BCLR clears the control FIFO (R01UM0007 §28 CFIFOCTR).
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
        usb.syscfg.modify(|_, w| w.dprpu()._0().usbe()._0());
        // SAFETY: mask all USBFS interrupt sources.
        usb.intenb0.write(|w| unsafe { w.bits(0) });
        cortex_m::peripheral::NVIC::mask(USB_IRQ);
        defmt::debug!("usb-cdc: device disabled");
    }

    async fn poll(&mut self) -> Event {
        poll_fn(|cx| {
            STATE.bus_waker.register(cx.waker());
            if !STATE.power_reported.swap(true, Ordering::AcqRel) {
                return Poll::Ready(Event::PowerDetected);
            }
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
        // The three CDC pipes; EP0/DCP is always active and needs no enable.
        let (ptype, mps, dir_in) = match pipe {
            BULK_OUT_PIPE => (PIPECFG_TYPE_BULK, BULK_MAX_PACKET, false),
            BULK_IN_PIPE => (PIPECFG_TYPE_BULK, BULK_MAX_PACKET, true),
            COMM_INT_PIPE => (PIPECFG_TYPE_INT, COMM_MAX_PACKET, true),
            _ => return,
        };
        let usb = regs();
        if enabled {
            let dir = if dir_in { PIPECFG_DIR_IN } else { 0 };
            // OUT bulk: NAK after a short packet so a partial frame ends the
            // transfer cleanly (unused in the print-only demo, correct anyway).
            let shtnak = if dir_in { 0 } else { PIPECFG_SHTNAK };
            // SAFETY: documented PIPESEL/PIPECFG/PIPEMAXP/CTR bring-up
            // (port of dcd_edpt_open); single owner, values per R01UM0007 §28.
            unsafe {
                usb.pipesel.write(|w| w.bits(u16::from(pipe)));
                usb.pipemaxp.write(|w| w.bits(mps));
                usb.pipecfg
                    .write(|w| w.bits(ptype | dir | shtnak | u16::from(pipe)));
                pipe_ctr_write(pipe, CTR_ACLRM | CTR_SQCLR);
                pipe_ctr_write(pipe, 0);
                pipe_ctr_write(pipe, PID_BUF);
            }
            if dir_in {
                usb.bempenb
                    .modify(|r, w| unsafe { w.bits(r.bits() | (1 << pipe)) });
            } else {
                usb.brdyenb
                    .modify(|r, w| unsafe { w.bits(r.bits() | (1 << pipe)) });
            }
            STATE.ep_enabled.fetch_or(1 << pipe, Ordering::AcqRel);
            STATE.ep_waker.wake();
        } else {
            usb.bempenb
                .modify(|r, w| unsafe { w.bits(r.bits() & !(1 << pipe)) });
            usb.brdyenb
                .modify(|r, w| unsafe { w.bits(r.bits() & !(1 << pipe)) });
            // SAFETY: PID_NAK stops the pipe.
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
        regs().dvstctr0.modify(|_, w| w.wkup()._1());
        Ok(())
    }
}

fn phy_init_fs() {
    const PHYSLEW: *mut u32 = 0x4009_00F0 as *mut u32;
    const DPUSR0R_FS: *mut u32 = 0x4009_0400 as *mut u32;
    const PHYSLEW_FS: u32 = 0x5;
    const DPUSR0R_FS_FIXPHY0: u32 = 1 << 4;
    // SAFETY: two documented USBFS-region registers the PAC omits; reachable
    // once UCLK is up (power_up_rail). FIXPHY0=0 puts the transceiver under
    // module control so DPRPU drives D+.
    unsafe {
        ptr::write_volatile(PHYSLEW, PHYSLEW_FS);
        let d = ptr::read_volatile(DPUSR0R_FS) & !DPUSR0R_FS_FIXPHY0;
        ptr::write_volatile(DPUSR0R_FS, d);
    }
}

// --- Control pipe (EP0 / DCP) — verbatim from usbfs.rs ----------------------

pub struct ControlPipe<'d> {
    max_packet_size: u16,
    _phantom: PhantomData<&'d ()>,
}

impl ControlPipe<'_> {
    fn ep0_send(&self, data: &[u8], last: bool) {
        let usb = regs();
        // SAFETY: CFIFOSEL for IN write, 16-bit.
        usb.cfifosel
            .write(|w| unsafe { w.bits(FIFOSEL_ISEL_WRITE | FIFOSEL_MBW_16BIT) });
        spin_until(|| usb.cfifosel.read().bits() & FIFOSEL_ISEL_WRITE != 0);

        let mut chunks = data.chunks_exact(2);
        for pair in &mut chunks {
            let word = u16::from(pair[0]) | (u16::from(pair[1]) << 8);
            // SAFETY: FIFO data port.
            usb.cfifo().write(|w| unsafe { w.bits(word) });
        }
        if let [tail] = chunks.remainder() {
            // SAFETY: 8-bit FIFO data port.
            usb.cfifol().write(|w| unsafe { w.bits(*tail) });
        }
        if data.len() < usize::from(self.max_packet_size) {
            // SAFETY: BVAL commits a short/zero packet.
            usb.cfifoctr.write(|w| unsafe { w.bits(FIFOCTR_BVAL) });
        }
        dcpctr_write(if last { CTR_CCPL | PID_BUF } else { PID_BUF });
    }

    fn ep0_recv(&self, buf: &mut [u8]) -> usize {
        let usb = regs();
        let dtln = usize::from(usb.cfifoctr.read().bits() & FIFOCTR_DTLN);
        let len = dtln.min(buf.len());
        for slot in buf.iter_mut().take(len) {
            *slot = usb.cfifol().read().bits();
        }
        // SAFETY: BCLR re-arms the receive buffer.
        usb.cfifoctr.write(|w| unsafe { w.bits(FIFOCTR_BCLR) });
        len
    }

    fn ep0_arm_out(&self) {
        let usb = regs();
        // SAFETY: CFIFOSEL for OUT read, 8-bit, pipe 0.
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
            // SAFETY: BCLR clears stale control-FIFO content.
            usb.cfifoctr.write(|w| unsafe { w.bits(FIFOCTR_BCLR) });
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
            // SAFETY: acknowledge VALID (W0C: 0 to VALID, 1 else).
            usb.intsts0.write(|w| unsafe { w.bits(!INTSTS0_VALID) });
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
        spin_until(|| regs().dcpctr.read().bits() & CTR_BSTS != 0);
        self.ep0_send(data, last);
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
        dcpctr_write(CTR_CCPL | PID_BUF);
    }

    async fn reject(&mut self) {
        let cur = regs().dcpctr.read().bits() & PID_MASK;
        dcpctr_write(cur | PID_STALL);
        dcpctr_write(PID_STALL);
    }

    async fn accept_set_address(&mut self, _addr: u8) {
        self.accept().await;
    }
}

// --- Endpoints --------------------------------------------------------------

/// IN endpoint (comm interrupt IN on PIPE6, or bulk IN on PIPE2). Transmits
/// via D0FIFO; completion on BEMP (buffer emptied = packet sent).
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
        let bit = 1u16 << self.pipe;
        poll_fn(|cx| {
            STATE.ep_waker.register(cx.waker());
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

        // Select D0FIFO for this pipe, 16-bit, and wait for FRDY (bounded).
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
            // SAFETY: 8-bit D0FIFO data port.
            usb.d0fifol().write(|w| unsafe { w.bits(*tail) });
        }
        // Commit a short/zero packet (a full-mps packet auto-commits when the
        // FIFO fills); ZLP handled by buf.len()==0 taking this branch too.
        if buf.len() < usize::from(self.info.max_packet_size) {
            // SAFETY: BVAL write-only control bit.
            usb.d0fifoctr.write(|w| unsafe { w.bits(FIFOCTR_BVAL) });
        }
        // Release the FIFO; wait for CURPIPE to clear.
        // SAFETY: documented D0FIFOSEL control write.
        usb.d0fifosel.write(|w| unsafe { w.bits(0) });
        spin_until(|| usb.d0fifosel.read().bits() & FIFOSEL_CURPIPE == 0);

        // Wait for the packet to be sent (BEMP for this pipe) or a bus reset.
        poll_fn(|cx| {
            STATE.ep_waker.register(cx.waker());
            if STATE.ep_enabled.load(Ordering::Acquire) & (1u16 << self.pipe) == 0 {
                return Poll::Ready(Err(EndpointError::Disabled));
            }
            if STATE.flags.take(F_BULK_IN) {
                Poll::Ready(Ok(()))
            } else {
                Poll::Pending
            }
        })
        .await
    }
}

/// Bulk OUT endpoint (host→device) on PIPE1. Receives via D1FIFO; completion
/// on BRDY. The print-only demo never calls [`Self::read`], but it is
/// implemented so the class behaves like a real serial port.
pub struct EndpointOut<'d> {
    info: EndpointInfo,
    pipe: u8,
    _phantom: PhantomData<&'d ()>,
}

impl Endpoint for EndpointOut<'_> {
    fn info(&self) -> &EndpointInfo {
        &self.info
    }

    async fn wait_enabled(&mut self) {
        let bit = 1u16 << self.pipe;
        poll_fn(|cx| {
            STATE.ep_waker.register(cx.waker());
            if STATE.ep_enabled.load(Ordering::Acquire) & bit != 0 {
                Poll::Ready(())
            } else {
                Poll::Pending
            }
        })
        .await
    }
}

impl EndpointOutTrait for EndpointOut<'_> {
    async fn read(&mut self, buf: &mut [u8]) -> Result<usize, EndpointError> {
        let usb = regs();
        let num = u16::from(self.pipe);

        // Wait until a packet has arrived for this pipe (BRDY) or the pipe is
        // disabled by a bus reset.
        poll_fn(|cx| {
            STATE.ep_waker.register(cx.waker());
            if STATE.ep_enabled.load(Ordering::Acquire) & (1u16 << self.pipe) == 0 {
                return Poll::Ready(Err(EndpointError::Disabled));
            }
            if STATE.flags.take(F_BULK_OUT) {
                Poll::Ready(Ok(()))
            } else {
                Poll::Pending
            }
        })
        .await?;

        // Select D1FIFO for this pipe, 8-bit read, and wait for FRDY.
        // SAFETY: documented D1FIFOSEL control write.
        usb.d1fifosel
            .write(|w| unsafe { w.bits(num | FIFOSEL_MBW_8BIT) });
        let ready = spin_until(|| {
            let sel = usb.d1fifosel.read().bits();
            (sel & FIFOSEL_CURPIPE) == num && (usb.d1fifoctr.read().bits() & FIFOCTR_FRDY != 0)
        });
        if !ready {
            // SAFETY: deselect before bailing.
            usb.d1fifosel.write(|w| unsafe { w.bits(0) });
            return Err(EndpointError::Disabled);
        }
        let dtln = usize::from(usb.d1fifoctr.read().bits() & FIFOCTR_DTLN);
        let len = dtln.min(buf.len());
        for slot in buf.iter_mut().take(len) {
            *slot = usb.d1fifol().read().bits();
        }
        // Clear the buffer to re-arm for the next OUT packet.
        // SAFETY: BCLR write-only control bit.
        usb.d1fifoctr.write(|w| unsafe { w.bits(FIFOCTR_BCLR) });
        // SAFETY: release the FIFO.
        usb.d1fifosel.write(|w| unsafe { w.bits(0) });
        spin_until(|| usb.d1fifosel.read().bits() & FIFOSEL_CURPIPE == 0);
        Ok(len)
    }
}
