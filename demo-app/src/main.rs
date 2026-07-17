//! Demo app — the image the bootloader flashes over CAN (U1.6,
//! docs/plan.md §6). Links at `APP_BASE = 0x8000` (docs/plan.md §1.2);
//! cortex-m-rt places `.vector_table` at the FLASH origin, and the
//! bootloader's jump sequence points VTOR/MSP there (R12) — the app never
//! touches VTOR itself.
//!
//! # Bench identity
//!
//! - LED "L" (D13 = P111) **double-blinks** (two 100 ms flashes, then a
//!   700 ms rest) — visibly different from the bootloader spike's steady
//!   1 Hz toggle, so the bench tells bootloader vs app at a glance.
//! - 1 s defmt heartbeat: `demo-app vX.Y (built ...)` + uptime.
//!
//! # Boot-to-bootloader trigger (plan §9 Q6, device side of R42)
//!
//! **D12 = P110** (port 1, pin 10) is configured as a GPIO input with the
//! internal pull-up. Grounding it — at startup or any time after — writes
//! the CAN-update magic `0xB007CAFE` to `VBTBKR[4..8]` (SPEC §1.4) and
//! issues `NVIC_SystemReset`, exercising the exact handoff path an Airform
//! `--enter-bootloader` RPC would use. Mapping evidence: TinyUSB's UNO R4
//! board support on `main` — `vendor/tinyusb/hw/bsp/ra/boards/uno_r4/board.h`
//! (`#define SW1 BSP_IO_PORT_01_PIN_10 // D12`, `BUTTON_STATE_ACTIVE 0`) —
//! already treats D12/P110 as the board's active-low input. D12 is free on
//! this project's bench: CAN uses D4/P103 + D5/P102, the LED is D13/P111
//! (docs/research/master-feature-spec.md §5.2).
//!
//! # Producing the `.bin` that `can_flash.py` flashes
//!
//! ```text
//! cargo build --release -p demo-app
//! rust-objcopy -O binary \
//!     target/thumbv7em-none-eabihf/release/demo-app \
//!     target/thumbv7em-none-eabihf/release/demo-app.bin
//! # (equivalently: llvm-objcopy, or `cargo objcopy --release -p demo-app
//! #  -- -O binary demo-app.bin` with cargo-binutils)
//! python tools/can-flash/can_flash.py demo-app.bin   # tool lives on `main`
//! ```
//!
//! The `.bin` is the raw app-slot image starting at `0x8000` — it carries
//! **no** CRC/metadata. The 16-byte metadata record at `0x0003_F800`
//! (`image_size`/`image_crc32`, SPEC §3.1) is computed and committed by the
//! bootloader's VERIFY/commit flow during the CAN session, never baked into
//! the `.bin`.

#![no_std]
#![no_main]

use defmt_rtt as _; // defmt transport over RTT
use panic_probe as _; // panic = defmt log, then reset

use cortex_m_rt::entry;
use embassy_executor::Executor;
use embassy_time::{Duration, Instant, Ticker, Timer};
use ra4m1_embassy::{clocks, pfs, time_driver, vbtbkr};
use static_cell::StaticCell;

/// Crate version, stamped into the heartbeat banner.
const VERSION: &str = env!("CARGO_PKG_VERSION");
/// UTC build timestamp, emitted by `build.rs` (`DEMO_APP_BUILT`).
const BUILT_AT: &str = env!("DEMO_APP_BUILT");

/// App-requested CAN-update handoff magic (SPEC §1.4): written to
/// `VBTBKR[4..8]` before reset; the bootloader reads + clears it and enters
/// CAN-only update mode.
const CAN_UPDATE_MAGIC: u32 = 0xB007_CAFE;

/// Double-blink cadence (bench identity, module docs): flash / gap / rest
/// give a 1.1 s pattern period. `Timer`-chain drift is irrelevant — this
/// identifies the image, it doesn't keep time.
const BLINK_FLASH: Duration = Duration::from_millis(100);
const BLINK_GAP: Duration = Duration::from_millis(100);
const BLINK_REST: Duration = Duration::from_millis(700);

/// Version-banner heartbeat cadence — slow enough to eyeball uptime drift.
const HEARTBEAT_PERIOD: Duration = Duration::from_secs(1);

/// Trigger-pin poll cadence: sized for a human grounding a jumper, not a
/// signal edge. Polling (vs an ICU pin interrupt) is a named deviation from
/// "await, don't poll" with the same rationale as the bootloader's CAN RX
/// ticker (docs/plan.md §9.8): the IELSR event-link bring-up is real work
/// with no benefit here, and the board is USB/bus powered, so the 10 Hz
/// wakeup costs nothing that matters.
const TRIGGER_POLL_PERIOD: Duration = Duration::from_millis(100);

/// Awaited before `sys_reset` so the probe-rs host drains the RTT buffer
/// and the trigger log line survives the reset.
const RTT_FLUSH_DELAY: Duration = Duration::from_millis(50);

/// UNO R4 Minima built-in LED "L": D13 = P111 (same pin the bootloader
/// drives; docs/research/master-feature-spec.md §2.5).
const LED_PORT: u8 = 1;
const LED_PIN: u8 = 11;

/// Trigger input D12 = P110 (module docs) as a mask into `PORT1.PIDR`.
const TRIGGER_PIN_MASK: u16 = 1 << 10;

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
    configure_trigger_pull_up(&p.PFS, &p.PMISC);
    time_driver::init();

    defmt::info!(
        "demo-app v{=str} (built {=str}) up at 0x8000 — double-blink on P111, \
         bootloader trigger on D12/P110 (ground to enter CAN update mode)",
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
        spawner.spawn(
            bootloader_trigger(p.PORT1, p.SYSTEM)
                .expect("bootloader_trigger pool empty: spawned twice"),
        );
    })
}

/// Configure the trigger pin (D12 = P110) as a GPIO input with the internal
/// pull-up (PmnPFS: PCR = 1, PDR = 0, PMR = 0), so it reads high until
/// deliberately grounded.
///
/// The PWPR unlock/re-lock pair mirrors `ra4m1_embassy::pfs`'s protocol
/// (R01UM0007, I/O Ports chapter, PWPR: B0WI guards PFSWE — two writes each
/// way). Duplicated here because that module keeps its unlock private and
/// only exposes output/peripheral-function writes; the PAC's typed
/// `p110pfs()` register keeps this raw-MMIO-free.
fn configure_trigger_pull_up(pfs_reg: &ra4m1::PFS, pmisc: &ra4m1::PMISC) {
    pmisc.pwpr.write(|w| w.b0wi()._0().pfswe()._0());
    pmisc.pwpr.write(|w| w.b0wi()._0().pfswe()._1());
    pfs_reg.p110pfs().write(|w| w.pcr()._1());
    pmisc.pwpr.write(|w| w.b0wi()._0().pfswe()._0());
    pmisc.pwpr.write(|w| w.b0wi()._1().pfswe()._0());
}

/// Bench-identity blink (module docs): two 100 ms flashes, then a 700 ms
/// rest — unmistakably different from the bootloader spike's steady toggle.
/// Owns PFS + PMISC outright (narrowest primitive: move).
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

/// 1 s version banner + uptime, the "which image is on the board" story.
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

/// Boot-to-bootloader trigger (module docs): sample D12/P110 immediately at
/// task start (≈ startup — the executor is up within milliseconds of reset)
/// and every [`TRIGGER_POLL_PERIOD`] after; when grounded, hand off to the
/// bootloader via `VBTBKR[4..8]` + system reset (SPEC §1.4). Owns PORT1
/// (input-data reads only — side-effect free, distinct registers from the
/// blink task's PmnPFS writes) and SYSTEM (VBTBKR).
#[embassy_executor::task]
async fn bootloader_trigger(port1: ra4m1::PORT1, system: ra4m1::SYSTEM) {
    let mut ticker = Ticker::every(TRIGGER_POLL_PERIOD);
    loop {
        if port1.pidr().read().pidr().bits() & TRIGGER_PIN_MASK == 0 {
            defmt::info!(
                "D12/P110 grounded — writing CAN-update magic to VBTBKR[4] and resetting \
                 into the bootloader"
            );
            vbtbkr::write_word(&system, vbtbkr::CAN_UPDATE_WORD_OFFSET, CAN_UPDATE_MAGIC);
            Timer::after(RTT_FLUSH_DELAY).await;
            cortex_m::peripheral::SCB::sys_reset();
        }
        ticker.next().await;
    }
}
