//! `blinky_gpio` — the M3.1 GPIO smoke example (plan §3/§7).
//!
//! Toggles the UNO R4 Minima built-in LED "L" (D13 = P111) at 1 Hz through the
//! HAL's [`ra4m1_hal::gpio::Output`] pin, driven by a normal thread-mode
//! Embassy executor with `embassy-time` running off the base GPT320 driver.
//! Proves the M3.0 scaffold end to end: base clocks + time driver +
//! `embedded-hal` `OutputPin`/`StatefulOutputPin` over the PORT block.
//!
//! Run on silicon: `cargo run --example blinky_gpio --release` (probe-rs
//! runner from `.cargo/config.toml`; links at the app base 0x8000 via this
//! crate's `memory.x`).

#![no_std]
#![no_main]

use defmt_rtt as _; // defmt transport over RTT
use panic_probe as _; // panic = defmt log, then reset

use cortex_m_rt::entry;
use embassy_executor::Executor;
use embassy_time::{Duration, Timer};
use embedded_hal::digital::StatefulOutputPin;
use ra4m1_hal::clock;
use ra4m1_hal::gpio::{Level, Output};
use static_cell::StaticCell;

/// Built-in LED "L": D13 = P111 (port 1, pin 11).
const LED_PORT: u8 = 1;
const LED_PIN: u8 = 11;
/// 1 Hz toggle → 500 ms half-period.
const HALF_PERIOD: Duration = Duration::from_millis(500);

static EXECUTOR: StaticCell<Executor> = StaticCell::new();

#[entry]
fn main() -> ! {
    // SAFETY: sole acquisition of the PAC singletons — once, from `#[entry]`,
    // before any other peripheral access exists.
    let p = unsafe { ra4m1::Peripherals::steal() };

    clock::init_hoco_48mhz(&p.SYSTEM);
    let led = Output::new(&p.PFS, &p.PMISC, LED_PORT, LED_PIN, Level::Low);
    ra4m1_hal::init_time();

    defmt::info!("ra4m1-hal blinky_gpio: toggling P111 at 1 Hz");

    let executor = EXECUTOR.init(Executor::new());
    executor.run(|spawner| {
        // Spawn failure is an init error (rules/embassy.md); the pool claim is
        // the fallible step in embassy-executor 0.10.
        spawner.spawn(blink(led).expect("blink pool empty: spawned twice"));
    })
}

/// Owns the LED pin outright (narrowest primitive: move) and toggles it every
/// half-period. Every loop iteration awaits a `Timer` — nothing busy-waits.
#[embassy_executor::task]
async fn blink(mut led: Output) {
    loop {
        // `Output` is infallible on this hardware, so the toggle cannot fail.
        led.toggle().expect("gpio toggle is Infallible");
        Timer::after(HALF_PERIOD).await;
    }
}
