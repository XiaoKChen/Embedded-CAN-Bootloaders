//! `spi` — SPI0 master loopback smoke (M3.3, plan §6), the runnable proof that
//! the SPI driver mirrors the spike **S-C** async pattern: a byte buffer is
//! exchanged under the executor with no busy-wait, the base GPT320
//! `embassy-time` driver (IEL0) still ticking.
//!
//! It brings up `SPI0` as an 8 MHz mode-0 master, binds its `SPRI`/`SPTI` lines
//! through [`ra4m1_hal::bind_interrupts`] to slots IEL5/IEL6 (clear of the
//! IEL2/3/4 the SCI example uses and IEL2 the ADC example uses), and runs an
//! async task that `transfer_in_place`s a rolling pattern once a second. Tie
//! **MOSI to MISO** (external loopback) and the received bytes equal the sent
//! ones; leave MISO floating and you still see the transfers clock out. A
//! separate heartbeat task proves time keeps running.
//!
//! Chip-select is the caller's job (this driver is a bare `SpiBus`); this smoke
//! test drives none, so it needs no CS wiring. For a real device wrap the bus in
//! [`embedded_hal_bus::spi::ExclusiveDevice`] with a GPIO CS.
//!
//! Pin / PSEL / ICU-event note (BENCH-VERIFY): the RSPCK0/MOSI0/MISO0
//! `(port, pin, psel)` values and the SPI0 ICU event numbers in
//! [`ra4m1_hal::spi`] are the FSP/datasheet values for this board and have
//! **not** been silicon-checked in this repo (parent bench-verifies via
//! M2.5/Renode). Confirm against R01UM0007 (I/O Ports "Selecting Peripheral
//! Functions" + ICU event table) and the UNO R4 Minima schematic before
//! trusting on hardware.
//!
//! Run: `cargo run --example spi --release`.

#![no_std]
#![no_main]

use defmt_rtt as _;
use panic_probe as _;

use cortex_m_rt::entry;
use embassy_executor::Executor;
use embassy_time::{Duration, Instant, Timer};
use embedded_hal_async::spi::SpiBus;
use ra4m1_hal::spi::{Config, Pin, RxInterruptHandler, Spi, Spi0, TxInterruptHandler};
use ra4m1_hal::{bind_interrupts, clock, interrupt};
use static_cell::StaticCell;

/// IELSR slots this example claims for SPI0 (kept clear of the SCI/ADC examples).
const RXI_SLOT: usize = 5; // IEL5 (SPRI0)
const TXI_SLOT: usize = 6; // IEL6 (SPTI0)

/// RSPCK0 / MOSI0 / MISO0 for the UNO R4 Minima — example values, BENCH-VERIFY.
/// PSEL 0x06 = SPI peripheral function (R01UM0007 pin-function table).
const RSPCK0: Pin = Pin {
    port: 1,
    pin: 2,
    psel: 0x06,
};
const MOSI0: Pin = Pin {
    port: 1,
    pin: 1,
    psel: 0x06,
};
const MISO0: Pin = Pin {
    port: 1,
    pin: 0,
    psel: 0x06,
};

bind_interrupts!(struct Irqs {
    IEL5 => RxInterruptHandler<Spi0, 5>;
    IEL6 => TxInterruptHandler<Spi0, 6>;
});

static EXECUTOR: StaticCell<Executor> = StaticCell::new();

#[entry]
fn main() -> ! {
    // SAFETY: sole PAC-singleton acquisition, once, from `#[entry]`.
    let p = unsafe { ra4m1::Peripherals::steal() };

    clock::init_hoco_48mhz(&p.SYSTEM);
    ra4m1_hal::init_time();

    let _irqs = Irqs; // construct the binding token (silences dead-code)

    let spi = Spi::<Spi0>::new(
        p.SPI0,
        &p.PFS,
        &p.PMISC,
        RSPCK0,
        MOSI0,
        MISO0,
        Config::new(8_000_000),
    );
    spi.enable_interrupts(RXI_SLOT, TXI_SLOT, interrupt::DEFAULT_APP_PRIORITY);

    defmt::info!("spi: SPI0 master @ 8 MHz mode 0; tie MOSI<->MISO to see loopback");

    let executor = EXECUTOR.init(Executor::new());
    executor.run(|spawner| {
        spawner.spawn(exercise(spi).expect("exercise pool empty: spawned twice"));
        spawner.spawn(heartbeat().expect("heartbeat pool empty: spawned twice"));
    })
}

/// Exchange a rolling 4-byte pattern once a second — the S-C no-busy-wait proof.
#[embassy_executor::task]
async fn exercise(mut spi: Spi<Spi0>) {
    let mut seed: u8 = 0xA5;
    loop {
        let mut buf = [
            seed,
            seed.wrapping_add(1),
            seed.wrapping_add(2),
            seed.wrapping_add(3),
        ];
        match spi.transfer_in_place(&mut buf).await {
            Ok(()) => {
                let _ = spi.flush().await;
                defmt::info!("spi rx {=[u8]:#04x}", buf);
            }
            Err(e) => defmt::warn!("spi error: {}", e),
        }
        seed = seed.wrapping_add(1);
        Timer::after(Duration::from_secs(1)).await;
    }
}

/// Prove `embassy-time` keeps ticking while the SPI task awaits hardware.
#[embassy_executor::task]
async fn heartbeat() {
    loop {
        Timer::after(Duration::from_secs(1)).await;
        defmt::info!("uptime {=u64} ms", Instant::now().as_millis());
    }
}
