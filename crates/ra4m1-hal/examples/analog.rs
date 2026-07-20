//! `analog` — the M3.4 ADC14 + DAC12 smoke example (plan §3/§7).
//!
//! Reads analog input **A0 (AN000 = P000)** with the ADC's async scan-complete
//! path (bound via [`ra4m1_hal::bind_interrupts`] to IEL2 — no busy-wait) and
//! mirrors it to the **DAC output DA0 (P014)**, scaling the 14-bit count down to
//! the DAC's 12-bit range. A 1 s defmt heartbeat prints the raw counts so the
//! bench sees the loop live while the base GPT320 `embassy-time` driver keeps
//! ticking (IEL0 undisturbed by the ADC's IEL2).
//!
//! Wire A0 to a potentiometer / voltage and scope DA0 to see it track.
//!
//! Run on silicon: `cargo run --example analog --release` (probe-rs runner from
//! `.cargo/config.toml`; links at the app base 0x8000 via this crate's
//! `memory.x`).

#![no_std]
#![no_main]

use defmt_rtt as _; // defmt transport over RTT
use panic_probe as _; // panic = defmt log, then reset

use cortex_m_rt::entry;
use embassy_executor::Executor;
use embassy_time::{Duration, Timer};
use ra4m1_hal::adc::{Adc, AdcScanInterruptHandler};
use ra4m1_hal::dac::Dac;
use ra4m1_hal::interrupt::{self, FIRST_APP_SLOT};
use ra4m1_hal::{bind_interrupts, clock, pfs};
use static_cell::StaticCell;

/// ADC channel A0 = AN000 (P000).
const A0_CHANNEL: u8 = 0;
const A0_PORT: u8 = 0;
const A0_PIN: u8 = 0;
/// IELSR slot claimed for the ADC scan-complete interrupt (first app slot).
const ADC_SLOT: usize = FIRST_APP_SLOT; // IEL2

static EXECUTOR: StaticCell<Executor> = StaticCell::new();

// Route the ADC scan-complete interrupt (S12ADI0) to IEL2 → the async waker.
bind_interrupts!(struct Irqs { IEL2 => AdcScanInterruptHandler<ADC_SLOT>; });

#[entry]
fn main() -> ! {
    // SAFETY: sole PAC-singleton acquisition, once, from `#[entry]`.
    let p = unsafe { ra4m1::Peripherals::steal() };

    clock::init_hoco_48mhz(&p.SYSTEM);

    // A0 (P000) as a floating analog input for the ADC.
    pfs::reset_to_gpio_input(&p.PFS, &p.PMISC, A0_PORT, A0_PIN);

    let mut adc = Adc::new(p.ADC140);
    let dac = Dac::new(p.DAC12, &p.PFS, &p.PMISC);

    ra4m1_hal::init_time();

    // Reference the binding struct so it is "constructed" (silences dead-code);
    // the strong IEL2 vector it generates is what the ADC's async read wakes on.
    let _irqs = Irqs;
    adc.enable_scan_interrupt(ADC_SLOT, interrupt::DEFAULT_APP_PRIORITY);

    defmt::info!("ra4m1-hal analog: A0 (AN000) -> DA0 (P014), async scan on IEL2");

    let executor = EXECUTOR.init(Executor::new());
    executor.run(|spawner| {
        spawner.spawn(mirror(adc, dac).expect("mirror pool empty: spawned twice"));
    })
}

/// Owns the ADC + DAC outright (narrowest primitive: move). Each iteration
/// awaits the scan-complete IRQ — nothing busy-waits — then mirrors A0 to DA0.
#[embassy_executor::task]
async fn mirror(mut adc: Adc, mut dac: Dac) {
    loop {
        match adc.read_async(A0_CHANNEL).await {
            // 14-bit count (0..=16383) down to the DAC's 12-bit range.
            Ok(raw) => {
                dac.set_value(raw >> 2);
                defmt::info!(
                    "A0 = {=u16} (14-bit) -> DA0 = {=u16} (12-bit)",
                    raw,
                    raw >> 2
                );
            }
            Err(e) => defmt::warn!("ADC read failed: {}", e),
        }
        Timer::after(Duration::from_secs(1)).await;
    }
}
