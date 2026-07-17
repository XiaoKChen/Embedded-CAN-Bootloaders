//! Bootloader entry point — boot decision, board bring-up, task spawning.
//!
//! U0.2 + U0.3 spike body (S0 + S5 + S2): clock bring-up with defmt-RTT
//! banner and LED on (S0), a VBTBKR round-trip across `NVIC_SystemReset`
//! (S5), then the Embassy executor with two toy tasks on the GPT320 time
//! driver (S2). S5 stays *before* the executor — the boot decision runs
//! pre-executor by design (docs/plan.md §4) and the S0/S5 bench checklist
//! must still be demonstrable from this one binary. U1.3 replaces the
//! decision logic with the real SPEC §2 order.

#![no_std]
#![no_main]

use defmt_rtt as _; // defmt transport over RTT
use panic_probe as _; // panic = defmt log, then reset

use cortex_m_rt::entry;
use embassy_executor::Executor;
use embassy_time::{Duration, Instant, Ticker};
use ra4m1_embassy::{pfs, time_driver, vbtbkr};
use static_cell::StaticCell;

mod board;
mod drivers;
mod flash_read;
mod ofs;
mod tasks;

/// App-requested CAN-update handoff magic (SPEC §1.4). Interim home for the
/// spike — the boot-decision constants move to `boot-logic` in U0.4.
const CAN_UPDATE_MAGIC: u32 = 0xB007_CAFE;

/// ~50 ms at ICLK 48 MHz: lets the probe-rs host drain the RTT buffer
/// before the S5 software reset tears the session down. Busy-wait is the
/// documented pre-executor exception (docs/plan.md §4) — nothing else runs.
const RTT_FLUSH_DELAY_CYCLES: u32 = 2_400_000;

/// S2 toy-task cadences (docs/plan.md §5): a bench-visible blink and a
/// heartbeat slow enough to eyeball against a wall clock for the ±1–2 %
/// HOCO-tolerance check.
const LED_BLINK_PERIOD: Duration = Duration::from_millis(500);
const HEARTBEAT_PERIOD: Duration = Duration::from_secs(1);

/// UNO R4 Minima built-in LED "L": D13 = P111 — same pin `board::init`
/// switches on at bring-up (its constants are private to the board seam;
/// the toy blink task dissolves into tasks/led.rs at U1.5).
const LED_PORT: u8 = 1;
const LED_PIN: u8 = 11;

/// Thread-mode executor (the only one — nothing here has a hard latency
/// requirement, docs/plan.md §4). Task memory in embassy-executor 0.10 is a
/// compiler-sized static pool per `#[task]` fn (pool_size 1 each), so the
/// RAM budget is exact per task with no arena to tune.
static EXECUTOR: StaticCell<Executor> = StaticCell::new();

#[entry]
fn main() -> ! {
    let board = board::init();

    defmt::info!("bootloader spike S0: HOCO 48 MHz up (ICLK 48 / PCLKA 48 / PCLKB 24 / FCLK 24)");
    defmt::info!(
        "  SCKSCR={=u8:#x} (expect 0x0 = HOCO), SCKDIVCR={=u32:#x} (expect 0x10040100)",
        board.system.sckscr.read().bits(),
        board.system.sckdivcr.read().bits(),
    );

    // S5: VBTBKR round-trip across a software reset (docs/plan.md §5).
    let handoff = vbtbkr::read_word(&board.system, vbtbkr::CAN_UPDATE_WORD_OFFSET);
    if handoff == CAN_UPDATE_MAGIC {
        defmt::info!("S5: CAN-update magic survived the software reset");
        vbtbkr::clear_word(&board.system, vbtbkr::CAN_UPDATE_WORD_OFFSET);
        let cleared = vbtbkr::read_word(&board.system, vbtbkr::CAN_UPDATE_WORD_OFFSET);
        if cleared == 0 {
            defmt::info!("S5 PASS: magic seen exactly once, word reads 0 after clear");
        } else {
            defmt::error!("S5 FAIL: VBTBKR word reads {=u32:#x} after clear", cleared);
        }
        // S0/S5 done — where U0.2 parked, S2 now starts the executor.
        run_spike_executor();
    }

    defmt::info!(
        "S5: VBTBKR[4..8]={=u32:#x} — writing magic, then NVIC_SystemReset",
        handoff
    );
    vbtbkr::write_word(
        &board.system,
        vbtbkr::CAN_UPDATE_WORD_OFFSET,
        CAN_UPDATE_MAGIC,
    );
    let written = vbtbkr::read_word(&board.system, vbtbkr::CAN_UPDATE_WORD_OFFSET);
    if written != CAN_UPDATE_MAGIC {
        defmt::error!("S5 FAIL: write read back {=u32:#x} — parking", written);
        loop {
            cortex_m::asm::wfi();
        }
    }

    cortex_m::asm::delay(RTT_FLUSH_DELAY_CYCLES);
    cortex_m::peripheral::SCB::sys_reset();
}

/// S2: bring the time base up and hand control to the executor forever
/// (it WFE-sleeps between wakes; once started it only ever dies by reset,
/// docs/plan.md §4).
fn run_spike_executor() -> ! {
    time_driver::init();

    // SAFETY: re-acquisition of the PAC singletons to hand PFS + PMISC to
    // the blink task, which owns them from here on. The only earlier PFS
    // access (`board::init`'s one-shot LED-on write) completed before this
    // point, and nothing else in the spike touches PFS/PMISC.
    let p = unsafe { ra4m1::Peripherals::steal() };

    defmt::info!("S2: executor starting — 500 ms blink + 1 s heartbeat (GPT320 @ 3 MHz)");

    let executor = EXECUTOR.init(Executor::new());
    executor.run(|spawner| {
        // Spawn failures are init errors — fail loudly (rules/embassy.md).
        // In embassy-executor 0.10 the task-pool claim is the fallible step.
        spawner.spawn(led_blink(p.PFS, p.PMISC).expect("led_blink pool empty: spawned twice"));
        spawner.spawn(heartbeat().expect("heartbeat pool empty: spawned twice"));
    })
}

/// S2 toy task: 500 ms LED toggle on P111. Bench check: a steady 1 Hz
/// blink proves `Ticker` scheduling end-to-end (driver → ISR → waker).
#[embassy_executor::task]
async fn led_blink(pfs_reg: ra4m1::PFS, pmisc: ra4m1::PMISC) {
    let mut ticker = Ticker::every(LED_BLINK_PERIOD);
    let mut led_on = false;
    loop {
        ticker.next().await;
        led_on = !led_on;
        pfs::set_gpio_output(&pfs_reg, &pmisc, LED_PORT, LED_PIN, led_on);
    }
}

/// S2 toy task: 1 s defmt heartbeat stamping `Instant::now()` so the bench
/// can eyeball tick accuracy against a wall clock (S2 pass: drift within
/// HOCO tolerance, ±~1–2 %).
#[embassy_executor::task]
async fn heartbeat() {
    let mut ticker = Ticker::every(HEARTBEAT_PERIOD);
    loop {
        ticker.next().await;
        let uptime = Instant::now();
        defmt::info!(
            "S2 heartbeat: uptime {=u64} ms ({=u64} ticks)",
            uptime.as_millis(),
            uptime.as_ticks()
        );
    }
}
