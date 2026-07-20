//! `i2c` — RIIC I2C bus scan (M3.3 U3.3.3 smoke), the runnable form of the
//! spike **S-C** pattern for I2C: an async transfer awaits real RIIC events
//! (TXI/RXI/TEI/EEI) under the executor with no busy-wait, while the base
//! GPT320 `embassy-time` driver (IEL0) keeps ticking.
//!
//! It brings up `IIC0` at 100 kHz (standard mode), binds its RXI/TXI/TEI/EEI
//! lines through [`ra4m1_hal::bind_interrupts`] to slots IEL5..IEL8, and runs an
//! async bus scanner: each 7-bit address `0x08..=0x77` gets a zero-length write
//! (ST + address + STOP); an ACK means a device answered, a
//! [`ra4m1_hal::i2c::Error::AddressNack`] means none did. A heartbeat task
//! proves time keeps running.
//!
//! Pin / PSEL / ICU-event note (BENCH-VERIFY): the SCL0/SDA0 `(port, pin, psel)`
//! values and the IIC0 ICU event numbers in `ra4m1_hal::i2c` are the
//! FSP/datasheet values for this board and have **not** been silicon-checked in
//! this repo (parent bench-verifies via M2.5/Renode). Confirm against R01UM0007
//! (I/O Ports "Selecting Peripheral Functions" + ICU event table) and the UNO
//! R4 Minima schematic before trusting on hardware.
//!
//! Run: `cargo run --example i2c --release`.

#![no_std]
#![no_main]

use defmt_rtt as _; // defmt transport over RTT
use panic_probe as _; // panic = defmt log, then reset

use cortex_m_rt::entry;
use embassy_executor::Executor;
use embassy_time::{Duration, Instant, Timer};
use embedded_hal_async::i2c::I2c as _;
use ra4m1_hal::i2c::{
    Config, EeiInterruptHandler, Error, I2c, Iic0, Pin, RxInterruptHandler, TeiInterruptHandler,
    TxInterruptHandler,
};
use ra4m1_hal::{bind_interrupts, clock, interrupt};
use static_cell::StaticCell;

/// IELSR slots this example claims for IIC0 (away from the uart/analog examples'
/// IEL2..IEL4).
const RXI_SLOT: usize = 5; // IEL5
const TXI_SLOT: usize = 6; // IEL6
const TEI_SLOT: usize = 7; // IEL7
const EEI_SLOT: usize = 8; // IEL8

/// SCL0 / SDA0 for UNO R4 Minima I2C — example values, BENCH-VERIFY.
/// PSEL 0x07 = IIC peripheral function (R01UM0007 pin-function table).
const SCL0: Pin = Pin {
    port: 4,
    pin: 0,
    psel: 0x07,
};
const SDA0: Pin = Pin {
    port: 4,
    pin: 1,
    psel: 0x07,
};

bind_interrupts!(struct Irqs {
    IEL5 => RxInterruptHandler<Iic0, 5>;
    IEL6 => TxInterruptHandler<Iic0, 6>;
    IEL7 => TeiInterruptHandler<Iic0, 7>;
    IEL8 => EeiInterruptHandler<Iic0, 8>;
});

static EXECUTOR: StaticCell<Executor> = StaticCell::new();

#[entry]
fn main() -> ! {
    // SAFETY: sole PAC-singleton acquisition, once, from `#[entry]`.
    let p = unsafe { ra4m1::Peripherals::steal() };

    clock::init_hoco_48mhz(&p.SYSTEM);
    ra4m1_hal::init_time();

    let _irqs = Irqs; // construct the binding token (silences dead-code)

    let i2c = I2c::<Iic0>::new(p.IIC0, &p.PFS, &p.PMISC, SCL0, SDA0, Config::standard());
    i2c.enable_interrupts(
        RXI_SLOT,
        TXI_SLOT,
        TEI_SLOT,
        EEI_SLOT,
        interrupt::DEFAULT_APP_PRIORITY,
    );

    defmt::info!("i2c: IIC0 @ 100 kHz, scanning the bus");

    let executor = EXECUTOR.init(Executor::new());
    executor.run(|spawner| {
        spawner.spawn(scan(i2c).expect("scan pool empty: spawned twice"));
        spawner.spawn(heartbeat().expect("heartbeat pool empty: spawned twice"));
    })
}

/// Scan the 7-bit address space once and log every device that ACKs — the S-C
/// no-busy-wait proof (each probe awaits RIIC events under the executor).
#[embassy_executor::task]
async fn scan(mut i2c: I2c<Iic0>) {
    let mut found = 0u32;
    for addr in 0x08u8..=0x77 {
        match i2c.write(addr, &[]).await {
            Ok(()) => {
                found += 1;
                defmt::info!("i2c: device found at {=u8:#04x}", addr);
            }
            Err(Error::AddressNack) => {} // no device at this address
            Err(e) => defmt::warn!("i2c: error probing {=u8:#04x}: {}", addr, e),
        }
    }
    defmt::info!("i2c: scan complete, {} device(s) found", found);
}

/// Prove `embassy-time` keeps ticking while the I2C task awaits hardware.
#[embassy_executor::task]
async fn heartbeat() {
    loop {
        Timer::after(Duration::from_secs(1)).await;
        defmt::info!("uptime {=u64} ms", Instant::now().as_millis());
    }
}
