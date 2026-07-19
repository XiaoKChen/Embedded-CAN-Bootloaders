//! `irq_smoke` — the spike **S-A** proof (plan §4.7/§5): the HAL's interrupt
//! binding composes with the base crate's `__INTERRUPTS`/`device.x` mechanism
//! and wakes a task, without editing `ra4m1-embassy`.
//!
//! It binds slot **IEL2** to a handler via [`ra4m1_hal::bind_interrupts`],
//! enables the NVIC line, then **software-pends** IEL2 (no external hardware
//! needed — `NVIC::pend` drives the line directly). The bound vector runs, the
//! handler signals a task, and the task wakes — all while the base GPT320
//! `embassy-time` driver (IEL0) keeps ticking. This is the runnable form of
//! the pass criteria; the link-time half of the proof is symbol inspection
//! (`llvm-nm`): a strong `IEL2` overrides `device.x`'s weak
//! `PROVIDE(IEL2 = DefaultHandler)` while `IEL0` (base time driver) stays
//! strong and `DefaultHandler` still backs the unbound slots.
//!
//! Run: `cargo run --example irq_smoke --release` — expect the "S-A PASS" log,
//! then a 1 s uptime heartbeat proving time still runs.

#![no_std]
#![no_main]

use defmt_rtt as _;
use panic_probe as _;

use cortex_m_rt::entry;
use embassy_executor::Executor;
use embassy_sync::blocking_mutex::raw::CriticalSectionRawMutex;
use embassy_sync::signal::Signal;
use embassy_time::{Duration, Instant, Timer};
use ra4m1_hal::interrupt::{self, InterruptHandler};
use ra4m1_hal::{bind_interrupts, clock};
use static_cell::StaticCell;

/// The app IELSR slot this proof claims (first slot available to HAL drivers).
const SMOKE_SLOT: usize = interrupt::FIRST_APP_SLOT; // IEL2

/// Set by the bound ISR, awaited by [`prover`] — "latest event wins" is all a
/// one-shot smoke needs (rules/embassy.md sharing order: Signal).
static IRQ_FIRED: Signal<CriticalSectionRawMutex, ()> = Signal::new();

static EXECUTOR: StaticCell<Executor> = StaticCell::new();

/// Minimal ISR (rules/embassy.md): the generated vector already cleared the
/// IELSR.IR latch; here we only wake the task and return.
struct SmokeHandler;
unsafe impl InterruptHandler for SmokeHandler {
    const SLOT: usize = SMOKE_SLOT;
    fn on_interrupt() {
        IRQ_FIRED.signal(());
    }
}

bind_interrupts!(struct Irqs { IEL2 => SmokeHandler; });

#[entry]
fn main() -> ! {
    // SAFETY: sole PAC-singleton acquisition, once, from `#[entry]`.
    let p = unsafe { ra4m1::Peripherals::steal() };

    clock::init_hoco_48mhz(&p.SYSTEM);
    ra4m1_hal::init_time();

    // Reference the generated binding struct so it is "constructed" (silences
    // dead-code); in a real driver `Irqs` is the token passed to a peripheral
    // constructor.
    let _irqs = Irqs;

    // Route + unmask IEL2. Event 0 = no ICU event linked: this smoke drives
    // the line by software pend, so no real peripheral event is needed.
    interrupt::enable(SMOKE_SLOT, 0, interrupt::DEFAULT_APP_PRIORITY);

    defmt::info!("irq_smoke: IEL2 bound + enabled; software-pending it from a task");

    let executor = EXECUTOR.init(Executor::new());
    executor.run(|spawner| {
        spawner.spawn(prover().expect("prover pool empty: spawned twice"));
    })
}

/// Software-pend IEL2, await the handler's signal, then heartbeat so the bench
/// sees `embassy-time` still running (no starvation, no inversion).
#[embassy_executor::task]
async fn prover() {
    cortex_m::peripheral::NVIC::pend(interrupt::slot_to_interrupt(SMOKE_SLOT));

    IRQ_FIRED.wait().await;
    defmt::info!(
        "S-A PASS: IEL2 strong vector fired and woke a task; IEL0 time driver undisturbed"
    );

    loop {
        Timer::after(Duration::from_secs(1)).await;
        defmt::info!(
            "embassy-time still ticking: uptime {=u64} ms",
            Instant::now().as_millis()
        );
    }
}
