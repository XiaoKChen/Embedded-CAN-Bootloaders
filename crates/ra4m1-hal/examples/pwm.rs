//! `pwm` — the M3.2 GPT PWM smoke example (plan §3/§7).
//!
//! Triangle-fades the UNO R4 Minima built-in LED "L" (D13 = P111 = GTIOC3A) at
//! 1 kHz through the HAL's [`ra4m1_hal::gpt::Pwm`] on channel 3 (`GPT163`),
//! driving duty via the `embedded-hal` 1.0 [`SetDutyCycle`] seam. `embassy-time`
//! runs off the base GPT320 driver (a *different* GPT channel — proving the
//! channel-0 reservation holds: the app PWM and the time base coexist).
//!
//! Run on silicon: `cargo run --example pwm --release` (probe-rs runner from
//! `.cargo/config.toml`; links at the app base 0x8000 via this crate's
//! `memory.x`). Cross-reference: the bootloader's `led.rs` fades the same pin
//! with the same GPT recipe.

#![no_std]
#![no_main]

use defmt_rtt as _; // defmt transport over RTT
use panic_probe as _; // panic = defmt log, then reset

use cortex_m_rt::entry;
use embassy_executor::Executor;
use embassy_time::{Duration, Ticker};
use embedded_hal::pwm::SetDutyCycle;
use ra4m1_hal::clock;
use ra4m1_hal::gpt::{Pwm, PwmOutput};
use static_cell::StaticCell;

/// Built-in LED "L": D13 = P111 (port 1, pin 11) = GTIOC3A → channel 3.
const LED_PORT: u8 = 1;
const LED_PIN: u8 = 11;
/// PWM carrier frequency — well above flicker, fits a 16-bit channel at ÷1.
const PWM_FREQ_HZ: u32 = 1_000;
/// Duty step cadence: 20 ms per step → a ~1 s ramp each way at 50 steps.
const STEP_PERIOD: Duration = Duration::from_millis(20);
/// Number of brightness steps across the full ramp.
const RAMP_STEPS: u16 = 50;

static EXECUTOR: StaticCell<Executor> = StaticCell::new();

#[entry]
fn main() -> ! {
    // SAFETY: sole acquisition of the PAC singletons — once, from `#[entry]`,
    // before any other peripheral access exists.
    let p = unsafe { ra4m1::Peripherals::steal() };

    clock::init_hoco_48mhz(&p.SYSTEM);
    // GPT163 (channel 3) is moved into the driver; GPT320 (channel 0) is left
    // for the time base below — the type system refuses it here anyway.
    let pwm = Pwm::new(
        p.GPT163,
        &p.PFS,
        &p.PMISC,
        LED_PORT,
        LED_PIN,
        PwmOutput::A,
        PWM_FREQ_HZ,
    )
    .expect("1 kHz PWM is reachable on a 16-bit GPT channel at 48 MHz PCLKD");
    ra4m1_hal::init_time();

    defmt::info!(
        "ra4m1-hal pwm: fading P111 (GPT ch3) at {=u32} Hz carrier",
        PWM_FREQ_HZ
    );

    let executor = EXECUTOR.init(Executor::new());
    executor.run(|spawner| {
        // Spawn failure is an init error (rules/embassy.md).
        spawner.spawn(fade(pwm).expect("fade pool empty: spawned twice"));
    })
}

/// Owns the PWM channel outright (narrowest primitive: move) and triangle-ramps
/// its duty forever. Every iteration awaits a `Ticker` — nothing busy-waits.
#[embassy_executor::task]
async fn fade(mut pwm: Pwm<ra4m1::GPT163>) {
    pwm.start();
    let max = pwm.max_duty_cycle();
    let step = (max / RAMP_STEPS).max(1);

    let mut level: u16 = 0;
    let mut rising = true;
    let mut ticker = Ticker::every(STEP_PERIOD);
    loop {
        ticker.next().await;
        // `set_duty_cycle` is `Infallible` on this driver (a register write).
        let _ = pwm.set_duty_cycle(level);

        if rising {
            level = level.saturating_add(step);
            if level >= max {
                level = max;
                rising = false;
            }
        } else {
            level = level.saturating_sub(step);
            if level <= step {
                level = 0;
                rising = true;
            }
        }
    }
}
