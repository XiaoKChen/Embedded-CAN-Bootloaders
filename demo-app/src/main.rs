//! Demo app — the image the bootloader flashes over CAN/USB-DFU (U1.6,
//! docs/plan.md §6). Links at `APP_BASE = 0x8000` (docs/plan.md §1.2);
//! cortex-m-rt places `.vector_table` at the FLASH origin, and the
//! bootloader's jump sequence points VTOR/MSP there (R12) — the app never
//! touches VTOR itself.
//!
//! # Bench identity
//!
//! - **USB-CDC serial**: enumerates a virtual COM port that prints
//!   `hello from RA4M1 CDC #N` once a second — the visible "I booted" signal
//!   on boards without the "L" LED. Uses the shared `ra4m1_embassy::usb`
//!   driver.
//! - 1 s defmt heartbeat over RTT: `demo-app vX.Y (built ...)` + uptime.
//! - LED "L" (D13 = P111) double-blinks (invisible on boards lacking it).
//!
//! # Entering the bootloader (no trigger pin)
//!
//! This app has **no** boot-to-bootloader trigger pin. Manual entry to the
//! bootloader/DFU is the stock UNO R4 Minima gesture — **double-tap reset** —
//! handled entirely bootloader-side (SPEC §2: a reset-button tap arms a
//! ~500 ms window; a second tap inside it lands in parked/DFU mode). The
//! programmatic "app requests the bootloader" path (write the CAN-update magic
//! to VBTBKR, then reset) is a bootloader feature the app can call when needed;
//! it is omitted here because it needs a free GPIO and D12/P110 is SPI MISO on
//! this board.
//!
//! # Producing the `.bin` the bootloader flashes
//!
//! ```text
//! cargo build --release -p demo-app
//! rust-objcopy -O binary \
//!     target/thumbv7em-none-eabihf/release/demo-app \
//!     target/thumbv7em-none-eabihf/release/demo-app.bin
//! # then flash over USB-DFU (dfu-util -a 0 -D demo-app.bin) or CAN.
//! ```
//!
//! The `.bin` is the raw app-slot image starting at `0x8000` — it carries
//! **no** CRC/metadata. The 16-byte metadata record at `0x0003_F800`
//! (`image_size`/`image_crc32`, SPEC §3.1) is computed and committed by the
//! bootloader's VERIFY/commit flow during the flash session, never baked into
//! the `.bin`.

#![no_std]
#![no_main]

use defmt_rtt as _; // defmt transport over RTT
use panic_probe as _; // panic = defmt log, then reset

use core::fmt::Write as _;

use cortex_m_rt::entry;
use embassy_executor::Executor;
use embassy_futures::join::join;
use embassy_time::{Duration, Instant, Ticker, Timer};
use embassy_usb::class::cdc_acm::{CdcAcmClass, State};
use embassy_usb::{Builder, Config};
use heapless::String;
use ra4m1_embassy::{clocks, pfs, time_driver, usb};
use static_cell::StaticCell;

/// USBFS interrupt vector (NVIC line IEL1) = the RA4M1 USB driver's ISR. The
/// shared `ra4m1_embassy::usb` module can't define this `#[no_mangle]` symbol
/// (both binaries link that crate and the bootloader has its own `IEL1`), so
/// each USB binary binds it here. Pulled into the link by
/// `time_driver::__INTERRUPTS[1]`, which references `IEL1`.
#[no_mangle]
extern "C" fn IEL1() {
    usb::on_interrupt();
}

/// Crate version, stamped into the heartbeat banner.
const VERSION: &str = env!("CARGO_PKG_VERSION");
/// UTC build timestamp, emitted by `build.rs` (`DEMO_APP_BUILT`).
const BUILT_AT: &str = env!("DEMO_APP_BUILT");

/// USB vendor ID (Arduino's, kept for familiarity).
const USB_VID: u16 = 0x2341;
/// USB product ID — **distinct** from the bootloader's DFU PID (0x0369) so the
/// host enumerates a fresh CDC COM port rather than reusing the DFU binding.
const USB_PID: u16 = 0x0370;

/// Double-blink cadence (bench identity): flash / gap / rest give a 1.1 s
/// pattern period. `Timer`-chain drift is irrelevant — this identifies the
/// image, it doesn't keep time.
const BLINK_FLASH: Duration = Duration::from_millis(100);
const BLINK_GAP: Duration = Duration::from_millis(100);
const BLINK_REST: Duration = Duration::from_millis(700);

/// Version-banner heartbeat cadence — slow enough to eyeball uptime drift.
const HEARTBEAT_PERIOD: Duration = Duration::from_secs(1);

/// CDC print cadence: one line per second.
const CDC_PRINT_PERIOD: Duration = Duration::from_secs(1);

/// UNO R4 Minima built-in LED "L": D13 = P111 (same pin the bootloader
/// drives; docs/research/master-feature-spec.md §2.5).
const LED_PORT: u8 = 1;
const LED_PIN: u8 = 11;

/// Thread-mode executor — same shape as the bootloader's (docs/plan.md §4):
/// nothing here has a hard latency requirement, and task memory in
/// embassy-executor 0.10 is a compiler-sized static pool per `#[task]` fn.
static EXECUTOR: StaticCell<Executor> = StaticCell::new();

#[entry]
fn main() -> ! {
    // SAFETY: sole acquisition of the PAC singletons — called exactly once,
    // from `#[entry]`, before any other peripheral access exists. (The
    // checked `Peripherals::take()` is not compiled in because the PAC's
    // `critical-section` feature is disabled.)
    let p = unsafe { ra4m1::Peripherals::steal() };

    clocks::init_hoco_48mhz(&p.SYSTEM);
    time_driver::init();

    // USB transceiver rail + UCLK before taking the USBFS peripheral.
    usb::power_up_rail();
    let usb_driver = usb::Driver::new(p.USBFS);

    defmt::info!(
        "demo-app v{=str} (built {=str}) up at 0x8000 — USB-CDC serial (COM port), \
         heartbeat on RTT, double-blink on P111",
        VERSION,
        BUILT_AT
    );

    let executor = EXECUTOR.init(Executor::new());
    executor.run(|spawner| {
        // Spawn failures are init errors — fail loudly (rules/embassy.md).
        // In embassy-executor 0.10 the task-pool claim is the fallible step.
        spawner.spawn(
            led_double_blink(p.PFS, p.PMISC).expect("led_double_blink pool empty: spawned twice"),
        );
        spawner.spawn(heartbeat().expect("heartbeat pool empty: spawned twice"));
        spawner.spawn(usb_cdc(usb_driver).expect("usb_cdc pool empty: spawned twice"));
    })
}

/// Bench-identity blink: two 100 ms flashes, then a 700 ms rest. Owns PFS +
/// PMISC outright (narrowest primitive: move). Invisible on boards without the
/// "L" LED, but harmless.
#[embassy_executor::task]
async fn led_double_blink(pfs_reg: ra4m1::PFS, pmisc: ra4m1::PMISC) {
    loop {
        for _ in 0..2 {
            pfs::set_gpio_output(&pfs_reg, &pmisc, LED_PORT, LED_PIN, true);
            Timer::after(BLINK_FLASH).await;
            pfs::set_gpio_output(&pfs_reg, &pmisc, LED_PORT, LED_PIN, false);
            Timer::after(BLINK_GAP).await;
        }
        Timer::after(BLINK_REST).await;
    }
}

/// 1 s version banner + uptime over RTT, the "which image is on the board" story.
#[embassy_executor::task]
async fn heartbeat() {
    let mut ticker = Ticker::every(HEARTBEAT_PERIOD);
    loop {
        ticker.next().await;
        defmt::info!(
            "demo-app v{=str} (built {=str}) — uptime {=u64} ms",
            VERSION,
            BUILT_AT,
            Instant::now().as_millis()
        );
    }
}

/// USB-CDC serial: enumerate a virtual COM port and print a counter line every
/// second — the visible "the bootloader flashed me and I booted" signal on an
/// LED-less board. Open the COM port and watch the lines tick. Uses the shared
/// `ra4m1_embassy::usb` driver.
#[embassy_executor::task]
async fn usb_cdc(driver: usb::Driver<'static>) {
    static CONFIG_DESC: StaticCell<[u8; 128]> = StaticCell::new();
    static BOS_DESC: StaticCell<[u8; 32]> = StaticCell::new();
    static CONTROL_BUF: StaticCell<[u8; 64]> = StaticCell::new();
    static STATE: StaticCell<State> = StaticCell::new();

    let mut config = Config::new(USB_VID, USB_PID);
    config.manufacturer = Some("Normal Corp");
    config.product = Some("RA4M1 CDC demo");
    config.serial_number = Some("DEMO-CDC");
    config.max_packet_size_0 = 64;
    // Windows CDC composite compatibility (embassy usb-serial example values).
    config.device_class = 0xEF;
    config.device_sub_class = 0x02;
    config.device_protocol = 0x01;
    config.composite_with_iads = true;

    let mut builder = Builder::new(
        driver,
        config,
        CONFIG_DESC.init([0; 128]),
        BOS_DESC.init([0; 32]),
        &mut [], // no MS OS descriptors
        CONTROL_BUF.init([0; 64]),
    );

    let mut class = CdcAcmClass::new(&mut builder, STATE.init(State::new()), 64);
    let mut usb_device = builder.build();

    let run = usb_device.run();
    let writer = async {
        let mut counter: u32 = 0;
        loop {
            class.wait_connection().await;
            defmt::info!("usb_cdc: host connected — streaming");
            loop {
                let mut line: String<64> = String::new();
                // Bounded write into a 64-byte line — always fits.
                let _ = write!(line, "hello from RA4M1 CDC #{}\r\n", counter);
                match class.write_packet(line.as_bytes()).await {
                    Ok(()) => {}
                    Err(e) => {
                        defmt::warn!("usb_cdc: write failed ({}) — awaiting reconnect", e);
                        break;
                    }
                }
                counter = counter.wrapping_add(1);
                Timer::after(CDC_PRINT_PERIOD).await;
            }
        }
    };
    join(run, writer).await;
}
