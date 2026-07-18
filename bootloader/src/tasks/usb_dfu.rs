//! USB DFU 1.1 function (device class) for parked/DFU mode (U2.2,
//! docs/plan.md §6 M2; SPEC §6.1–6.3, R36–R38 with the plan §1.2/§8 layout
//! values superseding R36's block counts).
//!
//! The bootloader **is** DFU mode — there is no run-time (appIDLE) half. The
//! module is fully generic over `D: embassy_usb::driver::Driver<'d>`; the
//! concrete USBFS driver is U2.1's and never appears here.
//!
//! # Pieces
//!
//! - [`machine`]: the pure DFU state machine over the [`DfuStorage`] seam —
//!   zero USB/HAL/executor deps, so U2.3 implements storage and the SPEC §6.3
//!   guards stay host-testable logic.
//! - [`DfuHandler`]: the embassy-usb [`Handler`] gluing control requests to
//!   the machine (one DFU interface, alt 0 `@CodeFlash` / alt 1 `@DataFlash`).
//! - [`register`]: writes the descriptors into a [`Builder`] and installs the
//!   handler.
//! - [`run_device`]: drives [`UsbDevice::run`] and performs the DETACH →
//!   drain → reset flow (SPEC §6.1).
//!
//! # U2.3 integration seam
//!
//! `#[embassy_executor::task]` functions cannot be generic, so U2.3 wraps
//! these pieces in a concrete task around the U2.1 driver type:
//!
//! ```ignore
//! #[embassy_executor::task]
//! async fn usb_dfu(driver: crate::drivers::usbfs::Driver<'static>) {
//!     static DETACH: DetachSignal = DetachSignal::new();
//!     // U2.3 storage impl: writable-window blocks over the shared
//!     // Mutex<NoopRawMutex, FlashLp>, reads via memory-mapped code flash.
//!     let mut handler = DfuHandler::new(storage, &DETACH);
//!     let mut builder = embassy_usb::Builder::new(
//!         driver,
//!         device_config(mfr, prod, serial), // names from the config block (R37)
//!         config_descriptor_buf,            // ≥ 64 B
//!         bos_descriptor_buf,               // ≥ 16 B
//!         &mut [],                          // no MS OS descriptors
//!         control_buf,                      // ≥ CONTROL_BUF_LEN (2 KB DNLOAD data stage)
//!     );
//!     register(&mut builder, &mut handler);
//!     let mut usb = builder.build();
//!     run_device(&mut usb, &DETACH).await
//! }
//! ```
//!
//! When the concrete driver's lifetime is `'static`, every `&'d mut` above
//! (buffers + handler) must be `'static` too — `StaticCell` per rules
//! (embassy.md), never `static mut`.
//!
//! # Blocking exceptions (docs/plan.md §4 class)
//!
//! The [`Handler`] hooks are synchronous, so — exactly like the C TinyUSB
//! callbacks — flash work runs inline in the control request:
//!
//! - each full DNLOAD chunk = one block erase + one block program (the same
//!   bounded, IRQ-masked FACI sessions can_update uses; R39 granularity);
//! - the manifest CRC pass reads back the whole image once (≤ ~0.27 s for a
//!   full slot at 48 MHz, once per completed download).
//!
//! No device-side timeouts by design: like the CAN session, the device is a
//! server parked on host commands; the host (dfu-util/Arduino IDE) owns every
//! retry/timeout budget. The only exits are the DETACH reset and an external
//! reset.

use core::future::{poll_fn, Future};
use core::pin::{pin, Pin};
use core::sync::atomic::{AtomicBool, Ordering};
use core::task::Poll;

use boot_logic::dfu_config::{self, DfuConfig};
use boot_logic::image_meta::{ImageRead, MetadataRecord};
use boot_logic::memory_map::{
    APP_BASE, BOOT_SIZE, CODE_FLASH_SIZE, FLASH_BLOCK_SIZE, METADATA_ADDR,
};
use embassy_time::{Duration, Timer};
use embassy_usb::control::{InResponse, OutResponse, Recipient, Request, RequestType};
use embassy_usb::driver::Driver;
use embassy_usb::types::{InterfaceNumber, StringIndex};
use embassy_usb::{Builder, Config, Handler, UsbDevice, UsbVersion};
use static_cell::StaticCell;

use crate::drivers::flash_lp::DATA_FLASH_BASE;
use crate::flash_read::CodeFlashReader;
use machine::{DfuAlt, DfuStorage, StorageError};

// --- Identity & descriptors (SPEC §6.1, R36) -----------------------------

/// USB vendor ID: Arduino.
pub const USB_VID: u16 = 0x2341;
/// USB product ID: UNO R4 Minima DFU bootloader.
pub const USB_PID: u16 = 0x0369;

/// Compiled-default manufacturer string; a valid config block overrides it
/// at enumeration (R37).
pub const DEFAULT_MANUFACTURER: &str = "Arduino";
/// Compiled-default product string (C: `BOARD_NAME " DFU"`); config-block
/// override as above (R37).
pub const DEFAULT_PRODUCT: &str = "UNO R4 Minima DFU";

/// Alt-setting 0 memory-layout string, `dfu-util`-style
/// (`<name>/<address>/<blocks>*<size><unit><mode>`).
///
/// Arithmetic (plan §1.2/§8 — supersedes R36's `8*2Ka,120*2Kg`):
/// - `16*2Ka` read-only blocks  = `BOOT_SIZE / FLASH_BLOCK_SIZE` = 32 KB / 2 KB = 16;
/// - `112*2Kg` erase/write blocks = 111 app blocks (`APP_BLOCK_COUNT`) + the
///   metadata block = 112 (uploads span all 112; *downloads* clamp at the
///   111-block app slot — the machine's SPEC §6.3 oversize guard — and the
///   metadata block is written only by the machine's manifest, R38);
/// - 16 + 112 = 128 blocks = the whole 256 KB code flash.
///
/// Const-asserted below so a memory-map edit cannot silently desync it.
pub const CODE_FLASH_LAYOUT: &str = "@CodeFlash /0x00000000/16*2Ka,112*2Kg";

/// Alt-setting 1 layout string, byte-identical to the C bootloader's
/// (8 × 1 KB data-flash blocks, SPEC §1.2). The `0x08000000` address is the
/// stock descriptor's value, kept frozen for tool parity — the RA4M1 data
/// flash actually sits at `0x4010_0000`.
pub const DATA_FLASH_LAYOUT: &str = "@DataFlash /0x08000000/8*1Kg";

const _: () = assert!(
    BOOT_SIZE / FLASH_BLOCK_SIZE == 16,
    "CODE_FLASH_LAYOUT '16*2Ka' desynced from the bootloader region"
);
const _: () = assert!(
    machine::WRITABLE_BLOCKS == 112,
    "CODE_FLASH_LAYOUT '112*2Kg' desynced from the app+metadata region"
);
const _: () = assert!(
    (16 + 112) * FLASH_BLOCK_SIZE == CODE_FLASH_SIZE,
    "CODE_FLASH_LAYOUT block counts must tile the code flash exactly"
);

/// DFU interface class triple (DFU 1.1 §4.2.3): application-specific /
/// DFU / **DFU-mode** protocol. The run-time protocol (`0x01`) is
/// deliberately absent — the bootloader is DFU mode itself.
const USB_CLASS_APPLICATION_SPECIFIC: u8 = 0xFE;
const DFU_SUBCLASS: u8 = 0x01;
const DFU_PROTOCOL_DFU_MODE: u8 = 0x02;

/// bDescriptorType of the DFU functional descriptor (DFU 1.1 §4.1.3).
const DESC_DFU_FUNCTIONAL: u8 = 0x21;

/// bmAttributes: bitCanDnload | bitCanUpload | bitManifestationTolerant —
/// the C build's `FUNC_ATTRS` exactly (no bitWillDetach: the host issues
/// DFU_DETACH and we reset, SPEC §6.1).
const DFU_ATTRIBUTES: u8 = 0x07;

/// wDetachTimeOut in ms (C parity).
const DETACH_TIMEOUT_MS: u16 = 1000;

/// wTransferSize: one flash erase block per DNLOAD, so every full chunk is
/// erase-one-block/program-one-block (R39 granularity). Deviation from the
/// C's 64 B EP0-packet-sized value, sanctioned by plan U2.2 — DFU control
/// transfers may span many EP0 packets.
const DFU_TRANSFER_SIZE: u16 = 2048;
const _: () = assert!(
    DFU_TRANSFER_SIZE as usize == machine::TRANSFER_SIZE,
    "wTransferSize must equal the machine's window/flash block size"
);

/// bcdDFUVersion 1.1.
const DFU_VERSION_BCD: u16 = 0x0110;

/// DFU functional descriptor body (bLength/bDescriptorType prepended by
/// embassy-usb): bmAttributes, wDetachTimeOut, wTransferSize, bcdDFUVersion.
const DFU_FUNCTIONAL_DESCRIPTOR: [u8; 7] = {
    let detach = DETACH_TIMEOUT_MS.to_le_bytes();
    let transfer = DFU_TRANSFER_SIZE.to_le_bytes();
    let version = DFU_VERSION_BCD.to_le_bytes();
    [
        DFU_ATTRIBUTES,
        detach[0],
        detach[1],
        transfer[0],
        transfer[1],
        version[0],
        version[1],
    ]
};

/// DFU 1.1 class request codes (§3).
const REQ_DETACH: u8 = 0;
const REQ_DNLOAD: u8 = 1;
const REQ_UPLOAD: u8 = 2;
const REQ_GETSTATUS: u8 = 3;
const REQ_CLRSTATUS: u8 = 4;
const REQ_GETSTATE: u8 = 5;
const REQ_ABORT: u8 = 6;

/// Minimum `control_buf` length for [`Builder::new`]: the DNLOAD data stage
/// is buffered whole by embassy-usb before the handler sees it.
pub const CONTROL_BUF_LEN: usize = machine::TRANSFER_SIZE;

/// How long [`run_device`] keeps servicing USB after DFU_DETACH before the
/// system reset — lets the status stage + host cleanup complete and flushes
/// RTT (C parity: "drain USB 100 ms → NVIC_SystemReset", SPEC §6.1).
const DETACH_DRAIN: Duration = Duration::from_millis(100);

// --- Device configuration (SPEC §6.1) ------------------------------------

/// embassy-usb device [`Config`] with the C bootloader's USB identity:
/// VID/PID `0x2341`/`0x0369`, bDeviceClass 0 (single-function device, no
/// IAD), bcdUSB 2.00, bcdDevice 1.00, EP0 64 B, bus-powered 100 mA.
///
/// `manufacturer`/`product` come from a valid DFU config block via
/// [`config_name`] (R37) and fall back to the compiled defaults here.
/// `serial` is the chip-ID hex string (SPEC §6.1); producing it needs
/// chip-unique-ID access, so U2.3 supplies it (`None` omits the descriptor
/// until then).
pub fn device_config<'a>(
    manufacturer: Option<&'a str>,
    product: Option<&'a str>,
    serial: Option<&'a str>,
) -> Config<'a> {
    let mut config = Config::new(USB_VID, USB_PID);
    // C descriptor parity: no CDC ⇒ class 0/0/0, plain USB 2.0 device.
    config.device_class = 0x00;
    config.device_sub_class = 0x00;
    config.device_protocol = 0x00;
    config.composite_with_iads = false;
    config.bcd_usb = UsbVersion::Two; // C: bcdUSB 0x0200
    config.device_release = 0x0100; // C: bcdDevice 0x0100
    config.manufacturer = Some(manufacturer.unwrap_or(DEFAULT_MANUFACTURER));
    config.product = Some(product.unwrap_or(DEFAULT_PRODUCT));
    config.serial_number = serial;
    config
}

/// View a config-block name field (`DfuConfig::{manufacturer,product}` +
/// its length) as the `&str` [`Config`] wants (R37). `None` — empty name or
/// non-UTF-8 garbage — means "use the compiled default".
pub fn config_name(name: &[u8; dfu_config::NAME_LEN], len: u8) -> Option<&str> {
    let len = usize::from(len).min(dfu_config::NAME_LEN);
    if len == 0 {
        return None;
    }
    core::str::from_utf8(&name[..len]).ok()
}

// --- Detach → reset plumbing (SPEC §6.1) ----------------------------------

/// DETACH latch shared between [`DfuHandler`] (setter) and [`run_device`]
/// (consumer).
///
/// A relaxed `AtomicBool` with no waker is sufficient **by construction**:
/// the flag is only ever set inside the handler, which runs inside
/// `UsbDevice::run`'s poll, and [`run_device`] re-checks it after every such
/// poll — a set can never be missed and never needs to wake anything.
pub struct DetachSignal {
    requested: AtomicBool,
}

impl DetachSignal {
    /// A fresh, un-requested signal (const: usable as a plain `static`).
    pub const fn new() -> Self {
        Self {
            requested: AtomicBool::new(false),
        }
    }

    fn request(&self) {
        self.requested.store(true, Ordering::Relaxed);
    }

    /// True once DFU_DETACH has been received.
    pub fn is_requested(&self) -> bool {
        self.requested.load(Ordering::Relaxed)
    }
}

impl Default for DetachSignal {
    fn default() -> Self {
        Self::new()
    }
}

// --- The embassy-usb handler ----------------------------------------------

/// Control-request glue between embassy-usb and the pure [`machine`]. Owns
/// the storage seam and the machine's 2 KB transfer window — U2.3 places it
/// somewhere `'static`-capable (see the module docs).
pub struct DfuHandler<'d, S: DfuStorage> {
    machine: machine::DfuMachine,
    storage: S,
    detach: &'d DetachSignal,
    /// Set by [`register`]; `None` = not registered ⇒ handles nothing.
    interface: Option<InterfaceNumber>,
    code_layout_index: Option<StringIndex>,
    data_layout_index: Option<StringIndex>,
}

impl<'d, S: DfuStorage> DfuHandler<'d, S> {
    /// A handler in `dfuIDLE` over `storage`, reporting DETACH on `detach`.
    pub fn new(storage: S, detach: &'d DetachSignal) -> Self {
        Self {
            machine: machine::DfuMachine::new(),
            storage,
            detach,
            interface: None,
            code_layout_index: None,
            data_layout_index: None,
        }
    }

    /// This request addresses our DFU interface (class request, interface
    /// recipient, wIndex = our interface number).
    fn handles(&self, req: &Request) -> bool {
        req.request_type == RequestType::Class
            && req.recipient == Recipient::Interface
            && self
                .interface
                .is_some_and(|iface| u16::from(u8::from(iface)) == req.index)
    }
}

fn accept_or_stall(accepted: bool) -> OutResponse {
    if accepted {
        OutResponse::Accepted
    } else {
        OutResponse::Rejected
    }
}

impl<S: DfuStorage> Handler for DfuHandler<'_, S> {
    fn reset(&mut self) {
        // USB bus reset = the host re-enumerated; a half-done session is void.
        self.machine.bus_reset();
    }

    fn set_alternate_setting(&mut self, iface: InterfaceNumber, alternate_setting: u8) {
        if Some(iface) == self.interface {
            self.machine.set_alt(alternate_setting);
        }
    }

    fn control_out(&mut self, req: Request, data: &[u8]) -> Option<OutResponse> {
        if !self.handles(&req) {
            return None;
        }
        Some(match req.request {
            REQ_DNLOAD => {
                // Blocking exception (module docs): one erase + one program
                // per full chunk, inline, like the C's tud_dfu_download_cb.
                let accepted = self.machine.dnload(&mut self.storage, data);
                if !accepted {
                    defmt::warn!(
                        "usb_dfu: DNLOAD rejected ({=usize} B at {=u32} B in): {}",
                        data.len(),
                        self.machine.received_len(),
                        self.machine.status()
                    );
                }
                accept_or_stall(accepted)
            }
            REQ_CLRSTATUS => accept_or_stall(self.machine.clr_status()),
            REQ_ABORT => accept_or_stall(self.machine.abort()),
            REQ_DETACH => {
                // SPEC §6.1 detach→reset: latch it; run_device drains
                // DETACH_DRAIN (status stage completes) and resets.
                defmt::info!("usb_dfu: DETACH — reset scheduled");
                self.detach.request();
                OutResponse::Accepted
            }
            _ => OutResponse::Rejected,
        })
    }

    fn control_in<'a>(&'a mut self, req: Request, buf: &'a mut [u8]) -> Option<InResponse<'a>> {
        if !self.handles(&req) {
            return None;
        }
        match req.request {
            REQ_GETSTATUS => {
                // Manifestation (metadata write-back, R38) runs inside this
                // call when a download just completed — module docs.
                let status = self.machine.get_status(&mut self.storage);
                let Some(out) = buf.get_mut(..status.len()) else {
                    return Some(InResponse::Rejected);
                };
                out.copy_from_slice(&status);
                Some(InResponse::Accepted(&buf[..status.len()]))
            }
            REQ_GETSTATE => {
                let Some(first) = buf.first_mut() else {
                    return Some(InResponse::Rejected);
                };
                *first = self.machine.get_state();
                Some(InResponse::Accepted(&buf[..1]))
            }
            REQ_UPLOAD => {
                let want = usize::from(req.length).min(buf.len());
                match self.machine.upload(&mut self.storage, &mut buf[..want]) {
                    Some(count) => Some(InResponse::Accepted(&buf[..count])),
                    None => Some(InResponse::Rejected),
                }
            }
            _ => Some(InResponse::Rejected),
        }
    }

    fn get_string(&mut self, index: StringIndex, _lang_id: u16) -> Option<&str> {
        if Some(index) == self.code_layout_index {
            Some(CODE_FLASH_LAYOUT)
        } else if Some(index) == self.data_layout_index {
            Some(DATA_FLASH_LAYOUT)
        } else {
            None
        }
    }
}

// --- Registration + run seam (U2.3 entry points) ---------------------------

/// Write the DFU interface into `builder` — one interface, alt 0
/// `@CodeFlash` / alt 1 `@DataFlash`, one DFU functional descriptor after
/// both (TinyUSB `TUD_DFU_DESCRIPTOR` layout) — and install `handler`.
///
/// Panics only at init time (rules/rust.md exception) when the builder's
/// `control_buf` cannot hold a wTransferSize DNLOAD data stage.
pub fn register<'d, D: Driver<'d>, S: DfuStorage + 'd>(
    builder: &mut Builder<'d, D>,
    handler: &'d mut DfuHandler<'d, S>,
) {
    assert!(
        builder.control_buf_len() >= CONTROL_BUF_LEN,
        "usb_dfu: control_buf must hold one wTransferSize (2048 B) DNLOAD data stage"
    );

    let mut func = builder.function(
        USB_CLASS_APPLICATION_SPECIFIC,
        DFU_SUBCLASS,
        DFU_PROTOCOL_DFU_MODE,
    );
    let mut iface = func.interface();
    handler.interface = Some(iface.interface_number());

    let code_layout = iface.string();
    let data_layout = iface.string();
    handler.code_layout_index = Some(code_layout);
    handler.data_layout_index = Some(data_layout);

    // Alt numbers are allocated in call order: 0 = @CodeFlash, 1 = @DataFlash.
    iface.alt_setting(
        USB_CLASS_APPLICATION_SPECIFIC,
        DFU_SUBCLASS,
        DFU_PROTOCOL_DFU_MODE,
        Some(code_layout),
    );
    let mut data_alt = iface.alt_setting(
        USB_CLASS_APPLICATION_SPECIFIC,
        DFU_SUBCLASS,
        DFU_PROTOCOL_DFU_MODE,
        Some(data_layout),
    );
    // Written after both interface descriptors — the TUD_DFU_DESCRIPTOR shape
    // hosts already parse (one functional descriptor for all alts).
    data_alt.descriptor(DESC_DFU_FUNCTIONAL, &DFU_FUNCTIONAL_DESCRIPTOR);

    drop(func);
    builder.handler(handler);
}

/// Serve USB DFU until a DETACH-triggered system reset (never returns).
///
/// **Not** a `#[embassy_executor::task]` — tasks cannot be generic over the
/// driver, so U2.3 wraps this in a concrete task fn (module docs).
///
/// No timeout on the USB awaits by design: the device is a server parked on
/// host commands (same story as the CAN session task). On DETACH the device
/// keeps servicing the bus for [`DETACH_DRAIN`] — the request's status stage
/// and host-side cleanup complete, and RTT flushes — then resets into the
/// boot decision, which validates and boots the downloaded image (mirrors
/// R13: never a direct jump out of update mode).
pub async fn run_device<'d, D: Driver<'d>>(usb: &mut UsbDevice<'d, D>, detach: &DetachSignal) -> ! {
    {
        let mut run = pin!(usb.run());
        let mut drain: Option<Timer> = None;
        poll_fn(|cx| {
            // usb.run() never resolves (`-> !`); polling it services the bus.
            // The handler — the only setter of `detach` — runs inside this
            // poll, so checking afterwards can never miss a request (see
            // DetachSignal). Once latched, keep polling the SAME run future
            // through the drain window: dropping it would abandon the
            // control pipe mid-status-stage.
            let _ = run.as_mut().poll(cx);
            if detach.is_requested() {
                let timer = drain.get_or_insert_with(|| Timer::after(DETACH_DRAIN));
                return Pin::new(timer).poll(cx);
            }
            Poll::Pending
        })
        .await;
    }
    defmt::info!("usb_dfu: DETACH drained — system reset");
    cortex_m::peripheral::SCB::sys_reset();
}

// --- U2.3 concrete task + flash storage ------------------------------------

/// Config-descriptor scratch (RAM, not flash — no size-gate cost). The DFU
/// configuration + one interface + two alt settings + the functional
/// descriptor total ~40 B; sized with margin.
const CONFIG_DESC_LEN: usize = 128;
/// BOS-descriptor scratch: no device capabilities are declared, so minimal.
const BOS_DESC_LEN: usize = 32;

/// [`DfuStorage`] over the shared [`crate::FlashMutex`] + memory-mapped code
/// flash — the U2.3 seam the [`machine`] was written against.
///
/// Guard-span discipline (docs/plan.md §4): every method acquires with
/// `try_lock`, does at most **one** block op (R39), and releases — never held
/// across an await (these are synchronous `fn`s called from the embassy-usb
/// control hooks). On one cooperative executor `try_lock` is total; a failure
/// would mean a guard was held across an await, so it is logged and fails
/// closed (the machine maps it to a DFU status code).
pub struct FlashDfuStorage {
    flash: &'static crate::FlashMutex,
}

impl FlashDfuStorage {
    /// Storage over the shared flash driver.
    pub fn new(flash: &'static crate::FlashMutex) -> Self {
        Self { flash }
    }
}

impl DfuStorage for FlashDfuStorage {
    fn erase_block(&mut self, block: u32) -> Result<(), StorageError> {
        // `block` < APP_BLOCK_COUNT (the machine guarantees it); offset from
        // APP_BASE. R39: exactly one 2 KB block erase per call.
        let addr = APP_BASE + block * FLASH_BLOCK_SIZE;
        let Ok(mut flash) = self.flash.try_lock() else {
            defmt::error!("usb_dfu: flash busy on erase (unreachable on one executor)");
            return Err(StorageError);
        };
        flash.erase_block(addr).map_err(|err| {
            defmt::error!("usb_dfu: erase {=u32:#x} failed ({})", addr, err);
            StorageError
        })
    }

    fn program_block(&mut self, block: u32, data: &[u8]) -> Result<(), StorageError> {
        // R39: one block program per call. `data` is in RAM (the machine's
        // window) and a multiple of the 8-byte write unit, satisfying the
        // flash driver's source-in-RAM + write-unit requirements.
        let addr = APP_BASE + block * FLASH_BLOCK_SIZE;
        let Ok(mut flash) = self.flash.try_lock() else {
            defmt::error!("usb_dfu: flash busy on program (unreachable on one executor)");
            return Err(StorageError);
        };
        flash.program_verified(addr, data).map_err(|err| {
            defmt::error!("usb_dfu: program block {=u32} failed ({})", block, err);
            StorageError
        })
    }

    fn read(&mut self, alt: DfuAlt, offset: u32, buf: &mut [u8]) -> Result<usize, StorageError> {
        let n = buf.len();
        match alt {
            // Code flash is memory-mapped and always readable — no mutex, no
            // P/E. `offset` is bytes from APP_BASE (the @CodeFlash region).
            DfuAlt::CodeFlash => CodeFlashReader
                .read(APP_BASE + offset, buf)
                .map(|()| n)
                .map_err(|_| StorageError),
            // Data flash needs the driver (the R34 gate). If unproven, the
            // read is refused → the upload short-frames out (fail-safe).
            DfuAlt::DataFlash => {
                let Ok(flash) = self.flash.try_lock() else {
                    defmt::error!("usb_dfu: flash busy on data-flash read (unreachable)");
                    return Err(StorageError);
                };
                flash
                    .read_data_flash(DATA_FLASH_BASE + offset, buf)
                    .map(|()| n)
                    .map_err(|_| StorageError)
            }
        }
    }

    fn write_metadata(&mut self, image_size: u32, image_crc32: u32) -> Result<(), StorageError> {
        // R38 commit: erase the metadata block THEN program the fresh record,
        // so stale metadata describing a previous image never survives a new
        // download (the trait contract). Two block ops back-to-back, both
        // synchronous, both released before returning.
        let record = MetadataRecord::for_image(image_size, image_crc32).to_bytes();
        let Ok(mut flash) = self.flash.try_lock() else {
            defmt::error!("usb_dfu: flash busy on metadata write (unreachable)");
            return Err(StorageError);
        };
        flash.erase_block(METADATA_ADDR).map_err(|err| {
            defmt::error!("usb_dfu: metadata erase failed ({})", err);
            StorageError
        })?;
        flash
            .program_verified(METADATA_ADDR, &record)
            .map_err(|err| {
                defmt::error!("usb_dfu: metadata write failed ({})", err);
                StorageError
            })
    }
}

/// The parked-mode USB DFU task (U2.3). `#[task]` fns can't be generic, so
/// this pins U2.1's concrete `Driver` and the [`FlashDfuStorage`] type around
/// the module's generic pieces (the integration seam in the module docs).
///
/// `driver` requires `usb_rail::power_up_transceiver_rail()` to have already
/// run — `main::run_update_mode` does it pre-executor for both update tasks.
/// `config` is the shared [`DfuConfig`]; the USB manufacturer/product strings
/// come from it (R37), never re-read from data flash.
#[embassy_executor::task]
pub async fn run(
    driver: crate::drivers::usbfs::Driver<'static>,
    flash: &'static crate::FlashMutex,
    config: &'static DfuConfig,
) {
    // Function-local `'static`s: one instance (pool_size 1), so each is
    // initialized exactly once (rules/embassy.md — StaticCell, never
    // `static mut`).
    static DETACH: DetachSignal = DetachSignal::new();
    static CONFIG_DESC: StaticCell<[u8; CONFIG_DESC_LEN]> = StaticCell::new();
    static BOS_DESC: StaticCell<[u8; BOS_DESC_LEN]> = StaticCell::new();
    static CONTROL_BUF: StaticCell<[u8; CONTROL_BUF_LEN]> = StaticCell::new();
    static HANDLER: StaticCell<DfuHandler<'static, FlashDfuStorage>> = StaticCell::new();

    let handler = HANDLER.init(DfuHandler::new(FlashDfuStorage::new(flash), &DETACH));

    let mut builder = Builder::new(
        driver,
        device_config(
            config_name(&config.manufacturer, config.manufacturer_len),
            config_name(&config.product, config.product_len),
            None, // serial (chip-ID hex) not wired yet — omits the descriptor
        ),
        CONFIG_DESC.init([0u8; CONFIG_DESC_LEN]),
        BOS_DESC.init([0u8; BOS_DESC_LEN]),
        &mut [], // no MS OS descriptors
        CONTROL_BUF.init([0u8; CONTROL_BUF_LEN]),
    );
    register(&mut builder, handler);
    let mut usb = builder.build();
    defmt::info!(
        "usb_dfu: enumerating (VID {=u16:#x} PID {=u16:#x})",
        USB_VID,
        USB_PID
    );
    run_device(&mut usb, &DETACH).await
}

// --- Pure state machine ----------------------------------------------------
// Keep `machine` + `tests` last and contiguous: they are extracted verbatim
// into a host crate for verification (see the tests module header).

pub mod machine {
    //! Pure DFU 1.1 DFU-mode state machine over the [`DfuStorage`] seam.
    //!
    //! Zero USB/HAL/executor dependencies (only `boot_logic` + `defmt`
    //! derives) so every SPEC §6.3 guard is plain host-testable logic.
    //! M3 candidate for hoisting into `crates/boot-logic` if the project
    //! wants standing host coverage (tests module header).
    //!
    //! State chart (DFU 1.1 §A.2, sync-completion shortcut — flash work
    //! finishes inside the request, so `dfuDNBUSY`/`dfuMANIFEST` are never
    //! entered and every GETSTATUS answers with bwPollTimeout = 0, the same
    //! shortcut the C/TinyUSB stack took):
    //!
    //! ```text
    //! dfuIDLE --DNLOAD(len>0: erase+program block)--> dfuDNLOAD-SYNC
    //!   dfuDNLOAD-SYNC --GETSTATUS--> dfuDNLOAD-IDLE
    //!   dfuDNLOAD-IDLE --DNLOAD(len>0)--> dfuDNLOAD-SYNC          (next block)
    //!   dfuDNLOAD-IDLE --DNLOAD(len=0)--> dfuMANIFEST-SYNC
    //!   dfuMANIFEST-SYNC --GETSTATUS: manifest (R38)--> dfuIDLE   (manifestation-tolerant)
    //! dfuIDLE --UPLOAD--> dfuUPLOAD-IDLE --short frame--> dfuIDLE
    //! any --guard/storage failure--> dfuERROR --CLRSTATUS--> dfuIDLE
    //! any (handler level) --DETACH--> drain + system reset (SPEC §6.1)
    //! ```
    //!
    //! Guards owned here (SPEC §6.3 / R38):
    //! - zero-length download (DNLOAD(0) straight from dfuIDLE) → `dfuERROR`;
    //! - writes beyond `APP_SLOT_SIZE` (111 blocks) → `errADDRESS`. Stricter
    //!   than the C, which let an oversized image spill through the metadata
    //!   block and merely skipped the write-back — plan U2.2 mandates the
    //!   error form, which also keeps the metadata block out of `erase_block`
    //!   / `program_block` reach entirely;
    //! - metadata written **only** on a successful manifest of a
    //!   `0 < len ≤ APP_SLOT_SIZE` image, with the CRC-32 recomputed from
    //!   flash; read-back/flash failures *skip* the write-back (the image
    //!   boots via the stock SP-check fallback — degraded validation, never
    //!   a brick).

    use boot_logic::crc32::Crc32;
    use boot_logic::memory_map::{
        APP_BLOCK_COUNT, APP_SLOT_SIZE, FLASH_BLOCK_SIZE, FLASH_WRITE_UNIT,
    };

    /// wTransferSize and the machine's window: exactly one erase block, so a
    /// full DNLOAD chunk is erase-one-block/program-one-block (R39-shaped).
    // Lossless: u32 → usize on this 32-bit target (and any ≥ 32-bit host).
    pub const TRANSFER_SIZE: usize = FLASH_BLOCK_SIZE as usize;

    /// The `112*2Kg` region of the layout string: the 111 app blocks plus
    /// the metadata block. This is the *descriptor/upload* extent only —
    /// downloads clamp at [`APP_SLOT_SIZE`] (the SPEC §6.3 oversize guard),
    /// so the metadata block is readable over DFU but written exclusively by
    /// [`DfuStorage::write_metadata`] at manifest (R38).
    pub const WRITABLE_BLOCKS: u32 = APP_BLOCK_COUNT + 1;

    /// Byte size of the `@CodeFlash` upload region (offset 0 = `APP_BASE`).
    pub const WRITABLE_SIZE: u32 = WRITABLE_BLOCKS * FLASH_BLOCK_SIZE;

    /// Data-flash size: 8 KB (SPEC §1.2) — the `@DataFlash` upload clamp.
    pub const DATA_FLASH_SIZE: u32 = 8 * 1024;

    /// A storage operation failed. Deliberately detail-free: the machine
    /// maps failures to DFU status codes by *which* operation failed;
    /// implementations log the specifics themselves.
    #[derive(Debug, Clone, Copy, PartialEq, Eq, defmt::Format)]
    pub struct StorageError;

    /// Which memory the host is addressing = the interface alt setting.
    #[derive(Debug, Clone, Copy, PartialEq, Eq, defmt::Format)]
    pub enum DfuAlt {
        /// Alt 0 `@CodeFlash`: offset 0 = `APP_BASE`. Downloads clamp at
        /// the 111-block app slot; uploads span the full 112-block window.
        CodeFlash,
        /// Alt 1 `@DataFlash`: offset 0 = data-flash base. Upload only —
        /// downloads are rejected (`errTARGET`). Deviation from the C, which
        /// mis-routed alt-1 writes into code flash through a Zephyr macro
        /// default; that bug is not replicated.
        DataFlash,
    }

    /// Storage seam the U2.3 integration implements over the shared
    /// (`Mutex<NoopRawMutex, FlashLp>`) flash driver + memory-mapped reads.
    ///
    /// Addressing: `block` counts 2 KB app-slot blocks from `APP_BASE` —
    /// always < [`APP_BLOCK_COUNT`]; the machine never erases or programs
    /// the metadata block through these (only [`Self::write_metadata`]
    /// touches it). `read` offsets are bytes from the region base
    /// (`APP_BASE` / data-flash base), never past the region size.
    pub trait DfuStorage {
        /// Erase app-slot block `block`.
        fn erase_block(&mut self, block: u32) -> Result<(), StorageError>;

        /// Program `data` at the start of freshly-erased block `block`.
        /// `data` is non-empty, a multiple of the 8-byte write unit,
        /// ≤ [`TRANSFER_SIZE`], and lives in RAM (the machine's window).
        fn program_block(&mut self, block: u32, data: &[u8]) -> Result<(), StorageError>;

        /// Fill `buf` from `offset` within `alt`'s region, returning how
        /// many bytes were read (short reads only at a region end).
        fn read(&mut self, alt: DfuAlt, offset: u32, buf: &mut [u8])
            -> Result<usize, StorageError>;

        /// R38 commit point: write a fresh metadata record for an image of
        /// `image_size` bytes with payload CRC `image_crc32`. The
        /// implementation MUST erase the metadata block before programming —
        /// stale metadata describing a previous image must never survive a
        /// new download (a valid-looking record with a mismatched payload
        /// CRC would park the board on the next boot, R10/R11).
        fn write_metadata(&mut self, image_size: u32, image_crc32: u32)
            -> Result<(), StorageError>;
    }

    /// DFU 1.1 device states (wire values, §A.2). Run-time (`app*`) states
    /// don't exist — the bootloader IS DFU mode. `dfuDNBUSY` (4) and
    /// `dfuMANIFEST` (7) are never entered (sync-completion shortcut, module
    /// docs).
    #[derive(Debug, Clone, Copy, PartialEq, Eq, defmt::Format)]
    #[repr(u8)]
    pub enum DfuState {
        /// Idle, ready for DNLOAD/UPLOAD.
        DfuIdle = 2,
        /// A DNLOAD chunk is programmed; awaiting the GETSTATUS poll.
        DnloadSync = 3,
        /// Mid-download, ready for the next chunk (or DNLOAD(0) = complete).
        DnloadIdle = 5,
        /// Download complete; the GETSTATUS poll runs manifestation (R38).
        ManifestSync = 6,
        /// Mid-upload.
        UploadIdle = 9,
        /// A guard or storage operation failed; see the status code.
        Error = 10,
    }

    /// DFU 1.1 status codes (§A.1) — the subset this machine reports.
    #[derive(Debug, Clone, Copy, PartialEq, Eq, defmt::Format)]
    #[repr(u8)]
    pub enum DfuStatus {
        /// No error.
        Ok = 0x00,
        /// Write to an unsupported target (download on `@DataFlash`).
        ErrTarget = 0x01,
        /// Block program failed.
        ErrWrite = 0x03,
        /// Block erase failed.
        ErrErase = 0x04,
        /// Write beyond the app slot (oversize image, SPEC §6.3) or a
        /// misaligned mid-stream chunk.
        ErrAddress = 0x08,
        /// Request invalid in the current state (incl. the zero-length
        /// download guard).
        ErrStalledPkt = 0x0F,
    }

    /// The DFU-mode state machine. Owns the 2 KB transfer window (tail
    /// padding to the 8-byte write unit + manifest CRC read-back reuse it),
    /// mirroring the C's `aucbuffer[PROGRAM_BLOCK_SIZE]`.
    pub struct DfuMachine {
        state: DfuState,
        status: DfuStatus,
        alt: DfuAlt,
        /// Bytes accepted this download — the manifest's `image_size` (R38).
        received_len: u32,
        upload_offset: u32,
        window: [u8; TRANSFER_SIZE],
    }

    impl DfuMachine {
        /// A machine in `dfuIDLE` on alt 0.
        pub fn new() -> Self {
            Self {
                state: DfuState::DfuIdle,
                status: DfuStatus::Ok,
                alt: DfuAlt::CodeFlash,
                received_len: 0,
                upload_offset: 0,
                window: [0xFF; TRANSFER_SIZE],
            }
        }

        /// Current state as the typed enum. Used by the host tests only —
        /// the handler answers GETSTATE via [`Self::get_state`] (the `u8`
        /// wire form) — so it is dead in the thumb build.
        #[allow(dead_code)]
        pub fn state(&self) -> DfuState {
            self.state
        }

        /// Current status code.
        pub fn status(&self) -> DfuStatus {
            self.status
        }

        /// Bytes accepted in the download in progress (diagnostics).
        pub fn received_len(&self) -> u32 {
            self.received_len
        }

        /// SET_INTERFACE: select the addressed memory. Alt numbers beyond
        /// the two we declared can't reach us (embassy-usb validates).
        pub fn set_alt(&mut self, alt: u8) {
            self.alt = match alt {
                0 => DfuAlt::CodeFlash,
                _ => DfuAlt::DataFlash,
            };
        }

        /// USB bus reset: the host re-enumerated; void any session.
        pub fn bus_reset(&mut self) {
            *self = Self::new();
        }

        /// DFU_DNLOAD. `data` is the request's data stage (`wLength` bytes;
        /// empty = "download complete"). Returns whether to ACK (`true`) or
        /// stall (`false` — the host then reads the code via GETSTATUS).
        pub fn dnload<S: DfuStorage>(&mut self, storage: &mut S, data: &[u8]) -> bool {
            match self.state {
                DfuState::DfuIdle => {
                    if data.is_empty() {
                        // SPEC §6.3 guard: a zero-length download (no data
                        // phase at all) is an error, never a manifest.
                        return self.fail(DfuStatus::ErrStalledPkt);
                    }
                    self.received_len = 0;
                    self.write_chunk(storage, data)
                }
                DfuState::DnloadIdle => {
                    if data.is_empty() {
                        // Download complete; manifest on the GETSTATUS poll.
                        self.state = DfuState::ManifestSync;
                        true
                    } else {
                        self.write_chunk(storage, data)
                    }
                }
                _ => self.fail(DfuStatus::ErrStalledPkt),
            }
        }

        /// One DNLOAD chunk: guards, tail padding, erase + program.
        fn write_chunk<S: DfuStorage>(&mut self, storage: &mut S, data: &[u8]) -> bool {
            if self.alt != DfuAlt::CodeFlash {
                return self.fail(DfuStatus::ErrTarget);
            }
            if data.len() > TRANSFER_SIZE {
                // Host exceeded its own advertised wTransferSize.
                return self.fail(DfuStatus::ErrStalledPkt);
            }
            // Lossless: ≤ TRANSFER_SIZE (2048), checked above.
            let len = data.len() as u32;
            if !self.received_len.is_multiple_of(FLASH_BLOCK_SIZE) {
                // An earlier short (non-final) chunk left us mid-block; the
                // stream no longer maps onto erase blocks.
                return self.fail(DfuStatus::ErrAddress);
            }
            let end = self.received_len.saturating_add(len);
            if end > APP_SLOT_SIZE {
                // SPEC §6.3 oversize guard (strict form, plan U2.2): the
                // image would outgrow the 111-block app slot. Rejecting the
                // chunk also fences the metadata block off from downloads.
                return self.fail(DfuStatus::ErrAddress);
            }
            let block = self.received_len / FLASH_BLOCK_SIZE;

            // Pad the tail to the 8-byte write unit with 0xFF (the erased-
            // flash value, C parity). padded ≤ TRANSFER_SIZE because the
            // window is a whole multiple of the write unit.
            let unit = FLASH_WRITE_UNIT as usize; // lossless: 8
            let padded = data.len().div_ceil(unit) * unit;
            self.window[..data.len()].copy_from_slice(data);
            self.window[data.len()..padded].fill(0xFF);

            if storage.erase_block(block).is_err() {
                return self.fail(DfuStatus::ErrErase);
            }
            if storage
                .program_block(block, &self.window[..padded])
                .is_err()
            {
                return self.fail(DfuStatus::ErrWrite);
            }
            self.received_len = end;
            self.state = DfuState::DnloadSync;
            true
        }

        /// DFU_GETSTATUS: the 6-byte `{bStatus, bwPollTimeout[3], bState,
        /// iString}` reply. Drives the SYNC-state transitions; bwPollTimeout
        /// is always 0 (flash work already completed inside the request).
        pub fn get_status<S: DfuStorage>(&mut self, storage: &mut S) -> [u8; 6] {
            match self.state {
                DfuState::DnloadSync => {
                    // The chunk was programmed synchronously in dnload().
                    self.state = DfuState::DnloadIdle;
                }
                DfuState::ManifestSync => {
                    // Manifestation runs here, synchronously (blocking
                    // exception: one CRC read-back pass over the image).
                    self.manifest(storage);
                    // bitManifestationTolerant: straight back to idle.
                    self.state = DfuState::DfuIdle;
                }
                _ => {}
            }
            [self.status as u8, 0, 0, 0, self.state as u8, 0]
        }

        /// DFU_GETSTATE: the 1-byte state reply.
        pub fn get_state(&self) -> u8 {
            self.state as u8
        }

        /// DFU_CLRSTATUS: leave `dfuERROR`. Invalid elsewhere (stall).
        pub fn clr_status(&mut self) -> bool {
            if self.state == DfuState::Error {
                self.state = DfuState::DfuIdle;
                self.status = DfuStatus::Ok;
                true
            } else {
                false
            }
        }

        /// DFU_ABORT: discard any transfer in progress and return to
        /// `dfuIDLE`. Invalid in `dfuERROR` (stall; CLRSTATUS is the exit).
        pub fn abort(&mut self) -> bool {
            if self.state == DfuState::Error {
                return false;
            }
            self.state = DfuState::DfuIdle;
            self.received_len = 0;
            self.upload_offset = 0;
            true
        }

        /// DFU_UPLOAD: fill `buf` from the current alt's region. Returns
        /// `Some(count)` to reply with `count` bytes (`count < buf.len()`
        /// is the short frame that ends the upload) or `None` to stall.
        pub fn upload<S: DfuStorage>(&mut self, storage: &mut S, buf: &mut [u8]) -> Option<usize> {
            match self.state {
                DfuState::DfuIdle => {
                    self.upload_offset = 0;
                    self.state = DfuState::UploadIdle;
                }
                DfuState::UploadIdle => {}
                _ => {
                    self.fail(DfuStatus::ErrStalledPkt);
                    return None;
                }
            }
            let region_size = match self.alt {
                DfuAlt::CodeFlash => WRITABLE_SIZE,
                DfuAlt::DataFlash => DATA_FLASH_SIZE,
            };
            // Clamp at the region end (the C read unbounded past flash —
            // that bug is not replicated); the short frame ends the upload.
            let remaining = region_size.saturating_sub(self.upload_offset);
            // Lossless: usize is ≥ 32 bits on target and host.
            let want = buf.len().min(remaining as usize);
            let got = if want == 0 {
                0
            } else {
                match storage.read(self.alt, self.upload_offset, &mut buf[..want]) {
                    Ok(got) => got.min(want),
                    Err(StorageError) => {
                        self.fail(DfuStatus::ErrStalledPkt);
                        return None;
                    }
                }
            };
            // Lossless: got ≤ want ≤ remaining (a u32).
            self.upload_offset = self.upload_offset.saturating_add(got as u32);
            if got < buf.len() {
                self.state = DfuState::DfuIdle;
            }
            Some(got)
        }

        /// Manifestation (R38 / SPEC §6.3): recompute the image CRC-32 from
        /// flash and commit a fresh metadata record — or skip, leaving the
        /// image on the stock SP-check fallback. Every early return is a
        /// documented skip; manifestation itself never errors the session.
        fn manifest<S: DfuStorage>(&mut self, storage: &mut S) {
            let image_len = self.received_len;
            // SPEC §6.3 guards, defence in depth: both are unreachable by
            // construction — DNLOAD(0) from dfuIDLE already errored the
            // zero-length case, and write_chunk's oversize guard bounds
            // received_len ≤ APP_SLOT_SIZE. Kept so manifest() is safe even
            // if the request plumbing above it ever changes.
            if image_len == 0 || image_len > APP_SLOT_SIZE {
                return;
            }
            let mut crc = Crc32::new();
            let mut offset = 0u32;
            while offset < image_len {
                let take = (image_len - offset).min(FLASH_BLOCK_SIZE);
                // Lossless: take ≤ FLASH_BLOCK_SIZE (2048).
                let chunk = &mut self.window[..take as usize];
                let Ok(got) = storage.read(DfuAlt::CodeFlash, offset, chunk) else {
                    return; // R38: any flash error ⇒ skip the write-back.
                };
                if got != chunk.len() {
                    return; // Short read inside the image: skip.
                }
                crc.update(chunk);
                offset += take;
            }
            // R38 commit: only here, with the recomputed CRC. A write
            // failure is a documented skip (SP fallback), not a DFU error.
            let _ = storage.write_metadata(image_len, crc.finalize());
        }

        /// Enter `dfuERROR` with `status` (keeping the *first* failure's
        /// code if already there) and report "stall".
        fn fail(&mut self, status: DfuStatus) -> bool {
            if self.state != DfuState::Error {
                self.status = status;
            }
            self.state = DfuState::Error;
            false
        }

        /// Test-only: force a manifest-pending state with an arbitrary
        /// received length, bypassing `dnload` — the only way to exercise
        /// the defence-in-depth guards at the commit point (unreachable
        /// through the request API, see [`Self::manifest`]).
        #[cfg(test)]
        pub(crate) fn force_manifest_pending(&mut self, received_len: u32) {
            self.received_len = received_len;
            self.state = DfuState::ManifestSync;
        }
    }

    impl Default for DfuMachine {
        fn default() -> Self {
            Self::new()
        }
    }
}

#[cfg(test)]
mod tests {
    //! Host tests for [`machine`]. The `bootloader` crate is a thumb-only
    //! binary (`[[bin]] test = false`), so `cargo test` never compiles these
    //! in-tree; they were verified by extracting `machine` + this module
    //! verbatim into a host crate (U2.2 verification notes — the two items
    //! are kept last and contiguous in this file for that reason). M3
    //! follow-up: hoist `machine` into `crates/boot-logic` for standing host
    //! coverage (it has zero USB/HAL deps by construction).

    use super::machine::*;
    use boot_logic::crc32::crc32;
    use boot_logic::memory_map::{APP_BLOCK_COUNT, APP_SLOT_SIZE};
    use std::vec::Vec;

    const BLOCK: usize = TRANSFER_SIZE;

    /// GETSTATUS byte offsets.
    const STATUS: usize = 0;
    const STATE: usize = 4;

    struct FakeStorage {
        image: Vec<u8>,
        data_flash: Vec<u8>,
        erased: Vec<bool>,
        metadata: Option<(u32, u32)>,
        fail_erase: bool,
        fail_program: bool,
        fail_read: bool,
        fail_metadata: bool,
        erase_log: Vec<u32>,
    }

    impl FakeStorage {
        fn new() -> Self {
            Self {
                image: vec![0u8; WRITABLE_SIZE as usize],
                data_flash: (0..DATA_FLASH_SIZE).map(|i| (i % 251) as u8).collect(),
                erased: vec![false; WRITABLE_BLOCKS as usize],
                metadata: None,
                fail_erase: false,
                fail_program: false,
                fail_read: false,
                fail_metadata: false,
                erase_log: Vec::new(),
            }
        }
    }

    impl DfuStorage for FakeStorage {
        fn erase_block(&mut self, block: u32) -> Result<(), StorageError> {
            if self.fail_erase {
                return Err(StorageError);
            }
            assert!(
                block < APP_BLOCK_COUNT,
                "machine must never erase the metadata block"
            );
            self.erase_log.push(block);
            self.erased[block as usize] = true;
            let start = block as usize * BLOCK;
            self.image[start..start + BLOCK].fill(0xFF);
            Ok(())
        }

        fn program_block(&mut self, block: u32, data: &[u8]) -> Result<(), StorageError> {
            if self.fail_program {
                return Err(StorageError);
            }
            assert!(
                block < APP_BLOCK_COUNT,
                "machine must never program the metadata block"
            );
            assert!(self.erased[block as usize], "program before erase");
            assert!(!data.is_empty() && data.len() <= BLOCK);
            assert_eq!(data.len() % 8, 0, "machine must pad to the write unit");
            let start = block as usize * BLOCK;
            self.image[start..start + data.len()].copy_from_slice(data);
            Ok(())
        }

        fn read(
            &mut self,
            alt: DfuAlt,
            offset: u32,
            buf: &mut [u8],
        ) -> Result<usize, StorageError> {
            if self.fail_read {
                return Err(StorageError);
            }
            let region: &[u8] = match alt {
                DfuAlt::CodeFlash => &self.image,
                DfuAlt::DataFlash => &self.data_flash,
            };
            let offset = offset as usize;
            assert!(
                offset + buf.len() <= region.len(),
                "machine read past the region"
            );
            buf.copy_from_slice(&region[offset..offset + buf.len()]);
            Ok(buf.len())
        }

        fn write_metadata(
            &mut self,
            image_size: u32,
            image_crc32: u32,
        ) -> Result<(), StorageError> {
            if self.fail_metadata {
                return Err(StorageError);
            }
            self.metadata = Some((image_size, image_crc32));
            Ok(())
        }
    }

    /// Drive one full chunked download (no manifest) asserting the
    /// SYNC→IDLE GETSTATUS handshake on every chunk.
    fn download(m: &mut DfuMachine, st: &mut FakeStorage, image: &[u8]) {
        for chunk in image.chunks(BLOCK) {
            assert!(m.dnload(st, chunk), "chunk rejected");
            assert_eq!(m.state(), DfuState::DnloadSync);
            let s = m.get_status(st);
            assert_eq!(s[STATUS], DfuStatus::Ok as u8);
            assert_eq!(s[STATE], DfuState::DnloadIdle as u8);
        }
    }

    /// Complete a download: DNLOAD(0) + the manifest GETSTATUS.
    fn manifest(m: &mut DfuMachine, st: &mut FakeStorage) -> [u8; 6] {
        assert!(m.dnload(st, &[]));
        assert_eq!(m.state(), DfuState::ManifestSync);
        m.get_status(st)
    }

    #[test]
    fn happy_download_pads_tail_and_commits_metadata_with_crc() {
        let (mut m, mut st) = (DfuMachine::new(), FakeStorage::new());
        let image: Vec<u8> = (0..2 * BLOCK + 100).map(|i| (i % 253) as u8).collect();
        download(&mut m, &mut st, &image);
        let s = manifest(&mut m, &mut st);
        assert_eq!(s[STATUS], DfuStatus::Ok as u8);
        assert_eq!(s[STATE], DfuState::DfuIdle as u8);
        // R38: metadata = (received length, CRC-32 of exactly those bytes).
        assert_eq!(st.metadata, Some((image.len() as u32, crc32(&image))));
        // Blocks erased in stream order; image bytes landed; tail padded
        // with 0xFF to the 8-byte write unit (100 → 104).
        assert_eq!(st.erase_log, vec![0, 1, 2]);
        assert_eq!(&st.image[..image.len()], &image[..]);
        assert_eq!(&st.image[2 * BLOCK + 100..2 * BLOCK + 104], &[0xFF; 4]);
    }

    #[test]
    fn zero_length_download_from_idle_is_an_error() {
        let (mut m, mut st) = (DfuMachine::new(), FakeStorage::new());
        assert!(!m.dnload(&mut st, &[]));
        assert_eq!(m.state(), DfuState::Error);
        let s = m.get_status(&mut st);
        assert_eq!(s[STATUS], DfuStatus::ErrStalledPkt as u8);
        assert_eq!(s[STATE], DfuState::Error as u8);
        assert_eq!(st.metadata, None);
    }

    #[test]
    fn oversize_download_rejected_at_the_app_slot_boundary() {
        let (mut m, mut st) = (DfuMachine::new(), FakeStorage::new());
        // SPEC §6.3 oversize guard (strict form): a full app slot (111
        // blocks) is accepted; the chunk that would spill into the metadata
        // block is rejected without any storage call.
        let full = vec![0xAB; APP_SLOT_SIZE as usize];
        download(&mut m, &mut st, &full);
        assert!(!m.dnload(&mut st, &[0u8; 8]));
        assert_eq!(m.status(), DfuStatus::ErrAddress);
        assert_eq!(m.state(), DfuState::Error);
        // The metadata block was never erased or programmed (the FakeStorage
        // asserts back this up for every test); only app blocks were touched.
        assert_eq!(st.erase_log.len() as u32, APP_BLOCK_COUNT);
        assert!(st.erase_log.iter().all(|&b| b < APP_BLOCK_COUNT));
        assert_eq!(st.metadata, None);
    }

    #[test]
    fn manifest_commits_an_image_of_exactly_app_slot_size() {
        let (mut m, mut st) = (DfuMachine::new(), FakeStorage::new());
        let image = vec![0xC3; APP_SLOT_SIZE as usize];
        download(&mut m, &mut st, &image);
        manifest(&mut m, &mut st);
        assert_eq!(st.metadata, Some((APP_SLOT_SIZE, crc32(&image))));
    }

    #[test]
    fn storage_failures_map_to_dfu_status_codes() {
        let (mut m, mut st) = (DfuMachine::new(), FakeStorage::new());
        st.fail_erase = true;
        assert!(!m.dnload(&mut st, &[0u8; BLOCK]));
        assert_eq!(m.status(), DfuStatus::ErrErase);

        let (mut m, mut st) = (DfuMachine::new(), FakeStorage::new());
        st.fail_program = true;
        assert!(!m.dnload(&mut st, &[0u8; BLOCK]));
        assert_eq!(m.status(), DfuStatus::ErrWrite);
    }

    #[test]
    fn manifest_flash_failures_skip_the_write_back_without_erroring() {
        for fail_read in [true, false] {
            let (mut m, mut st) = (DfuMachine::new(), FakeStorage::new());
            download(&mut m, &mut st, &[0x11; BLOCK]);
            st.fail_read = fail_read;
            st.fail_metadata = !fail_read;
            let s = manifest(&mut m, &mut st);
            // R38: degraded to the SP fallback, never a DFU error.
            assert_eq!(s[STATUS], DfuStatus::Ok as u8);
            assert_eq!(s[STATE], DfuState::DfuIdle as u8);
            assert_eq!(st.metadata, None);
        }
    }

    #[test]
    fn clr_status_recovers_and_a_fresh_download_restarts_at_block_zero() {
        let (mut m, mut st) = (DfuMachine::new(), FakeStorage::new());
        assert!(!m.dnload(&mut st, &[])); // force dfuERROR
        assert!(!m.abort(), "ABORT is invalid in dfuERROR");
        assert!(m.clr_status());
        assert_eq!(m.state(), DfuState::DfuIdle);
        assert_eq!(m.status(), DfuStatus::Ok);
        download(&mut m, &mut st, &[0x22; BLOCK]);
        assert_eq!(st.erase_log, vec![0]);
    }

    #[test]
    fn abort_discards_the_download_and_the_next_one_restarts() {
        let (mut m, mut st) = (DfuMachine::new(), FakeStorage::new());
        download(&mut m, &mut st, &[0x33; 2 * BLOCK]);
        assert!(m.abort());
        assert_eq!(m.state(), DfuState::DfuIdle);
        download(&mut m, &mut st, &[0x44; BLOCK]);
        assert_eq!(st.erase_log, vec![0, 1, 0]); // restarted at block 0
        manifest(&mut m, &mut st);
        assert_eq!(st.metadata, Some((BLOCK as u32, crc32(&[0x44; BLOCK]))));
    }

    #[test]
    fn upload_streams_code_flash_and_short_frames_at_the_window_end() {
        let (mut m, mut st) = (DfuMachine::new(), FakeStorage::new());
        for (i, byte) in st.image.iter_mut().enumerate() {
            *byte = (i % 249) as u8;
        }
        let mut collected = Vec::new();
        let mut buf = [0u8; TRANSFER_SIZE];
        loop {
            let got = m.upload(&mut st, &mut buf).expect("upload stalled");
            collected.extend_from_slice(&buf[..got]);
            if got < buf.len() {
                break;
            }
        }
        assert_eq!(collected, st.image); // exactly the writable window
        assert_eq!(m.state(), DfuState::DfuIdle);
    }

    #[test]
    fn upload_on_the_dataflash_alt_clamps_at_8_kib() {
        let (mut m, mut st) = (DfuMachine::new(), FakeStorage::new());
        m.set_alt(1);
        let mut collected = Vec::new();
        let mut buf = [0u8; TRANSFER_SIZE];
        loop {
            let got = m.upload(&mut st, &mut buf).expect("upload stalled");
            collected.extend_from_slice(&buf[..got]);
            if got < buf.len() {
                break;
            }
        }
        assert_eq!(collected, st.data_flash);
    }

    #[test]
    fn download_on_the_dataflash_alt_is_rejected() {
        let (mut m, mut st) = (DfuMachine::new(), FakeStorage::new());
        m.set_alt(1);
        assert!(!m.dnload(&mut st, &[0u8; BLOCK]));
        assert_eq!(m.status(), DfuStatus::ErrTarget);
    }

    #[test]
    fn data_after_a_short_mid_stream_chunk_is_an_address_error() {
        let (mut m, mut st) = (DfuMachine::new(), FakeStorage::new());
        download(&mut m, &mut st, &[0x55; 100]); // short (final-looking) chunk
        assert!(!m.dnload(&mut st, &[0x66; BLOCK])); // ...followed by more data
        assert_eq!(m.status(), DfuStatus::ErrAddress);
    }

    #[test]
    fn requests_invalid_in_the_current_state_stall_into_error() {
        // DNLOAD while uploading.
        let (mut m, mut st) = (DfuMachine::new(), FakeStorage::new());
        let mut buf = [0u8; 64];
        m.upload(&mut st, &mut buf).expect("upload");
        assert_eq!(m.state(), DfuState::UploadIdle);
        assert!(!m.dnload(&mut st, &[0u8; 8]));
        assert_eq!(m.state(), DfuState::Error);

        // UPLOAD mid-download; the first failure's code is kept.
        let (mut m, mut st) = (DfuMachine::new(), FakeStorage::new());
        download(&mut m, &mut st, &[0x77; BLOCK]);
        assert!(m.upload(&mut st, &mut buf).is_none());
        assert_eq!(m.status(), DfuStatus::ErrStalledPkt);
        st.fail_erase = true;
        assert!(!m.dnload(&mut st, &[0u8; 8]));
        assert_eq!(m.status(), DfuStatus::ErrStalledPkt); // first cause kept
    }

    #[test]
    fn bus_reset_returns_to_idle_from_anywhere() {
        let (mut m, mut st) = (DfuMachine::new(), FakeStorage::new());
        download(&mut m, &mut st, &[0x88; BLOCK]);
        m.bus_reset();
        assert_eq!(m.state(), DfuState::DfuIdle);
        assert_eq!(m.status(), DfuStatus::Ok);
        assert_eq!(m.received_len(), 0);
    }

    #[test]
    fn get_status_reports_without_side_effects_in_stable_states() {
        let (mut m, mut st) = (DfuMachine::new(), FakeStorage::new());
        let s = m.get_status(&mut st);
        assert_eq!(s, [0, 0, 0, 0, DfuState::DfuIdle as u8, 0]);
        assert_eq!(m.get_state(), DfuState::DfuIdle as u8);
        assert!(!m.clr_status(), "CLRSTATUS outside dfuERROR stalls");
    }
}
