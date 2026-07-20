//! `uart` — SCI UART echo (M3.3, plan §6 U3.3.1 smoke), the runnable form of
//! the spike **S-C** pass criterion: a byte echoes host<->device under the
//! executor with no busy-wait, the base GPT320 `embassy-time` driver (IEL0)
//! still ticking.
//!
//! It brings up `SCI2` (the UNO R4 Minima `Serial1` on D0/D1) in async UART
//! mode at 115200 8N1, binds its RXI/TXI/TEI lines through
//! [`ra4m1_hal::bind_interrupts`] to slots IEL2/IEL3/IEL4, and runs an async
//! echo task: every received byte is written back and flushed, all via
//! `embedded_io_async`. A separate heartbeat task proves time keeps running.
//!
//! Pin / PSEL / ICU-event note (BENCH-VERIFY): the TXD2/RXD2 `(port, pin,
//! psel)` values and the SCI2 ICU event numbers in `ra4m1_hal::sci` are the
//! FSP/datasheet values for this board and have not been silicon-checked in
//! this repo (parent bench-verifies via M2.5/Renode). Confirm against
//! R01UM0007 (I/O Ports "Selecting Peripheral Functions" + ICU event table)
//! and the UNO R4 Minima schematic before trusting on hardware.
//!
//! Run: `cargo run --example uart --release`, then type into the serial port.

#![no_std]
#![no_main]

use defmt_rtt as _;
use panic_probe as _;

use cortex_m_rt::entry;
use embassy_executor::Executor;
use embassy_time::{Duration, Instant, Timer};
use embedded_io_async::{Read, Write};
use ra4m1_hal::sci::{
    Config, Pin, RxInterruptHandler, Sci2, TeiInterruptHandler, TxInterruptHandler, Uart,
};
use ra4m1_hal::{bind_interrupts, clock, interrupt};
use static_cell::StaticCell;

/// IELSR slots this example claims for SCI2 (first slots free to HAL drivers).
const RXI_SLOT: usize = 2; // IEL2
const TXI_SLOT: usize = 3; // IEL3
const TEI_SLOT: usize = 4; // IEL4

/// TXD2 / RXD2 for UNO R4 Minima `Serial1` — example values, BENCH-VERIFY.
/// PSEL 0x04 = SCI peripheral function (R01UM0007 pin-function table).
const TXD2: Pin = Pin {
    port: 3,
    pin: 1,
    psel: 0x04,
};
const RXD2: Pin = Pin {
    port: 3,
    pin: 2,
    psel: 0x04,
};

bind_interrupts!(struct Irqs {
    IEL2 => RxInterruptHandler<Sci2, 2>;
    IEL3 => TxInterruptHandler<Sci2, 3>;
    IEL4 => TeiInterruptHandler<Sci2, 4>;
});

static EXECUTOR: StaticCell<Executor> = StaticCell::new();

#[entry]
fn main() -> ! {
    // SAFETY: sole PAC-singleton acquisition, once, from `#[entry]`.
    let p = unsafe { ra4m1::Peripherals::steal() };

    clock::init_hoco_48mhz(&p.SYSTEM);
    ra4m1_hal::init_time();

    let _irqs = Irqs; // construct the binding token (silences dead-code)

    let uart = Uart::<Sci2>::new(p.SCI2, &p.PFS, &p.PMISC, TXD2, RXD2, Config::new(115_200));
    uart.enable_interrupts(
        RXI_SLOT,
        TXI_SLOT,
        TEI_SLOT,
        interrupt::DEFAULT_APP_PRIORITY,
    );

    defmt::info!("uart: SCI2 @ 115200 8N1, echo on D0/D1; type to see bytes come back");

    let executor = EXECUTOR.init(Executor::new());
    executor.run(|spawner| {
        spawner.spawn(echo(uart).expect("echo pool empty: spawned twice"));
        spawner.spawn(heartbeat().expect("heartbeat pool empty: spawned twice"));
    })
}

/// Echo every received byte back to the host — the S-C no-busy-wait proof.
#[embassy_executor::task]
async fn echo(mut uart: Uart<Sci2>) {
    let mut buf = [0u8; 32];
    loop {
        match uart.read(&mut buf).await {
            Ok(n) => {
                if uart.write_all(&buf[..n]).await.is_ok() {
                    let _ = uart.flush().await;
                }
            }
            Err(e) => defmt::warn!("uart rx error: {}", e),
        }
    }
}

/// Prove `embassy-time` keeps ticking while the UART tasks await hardware.
#[embassy_executor::task]
async fn heartbeat() {
    loop {
        Timer::after(Duration::from_secs(1)).await;
        defmt::info!("uptime {=u64} ms", Instant::now().as_millis());
    }
}
