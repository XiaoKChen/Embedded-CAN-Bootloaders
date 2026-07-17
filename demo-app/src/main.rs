//! Demo app entry point — links at `0x8000` (docs/plan.md §1.2), inside the
//! bootloader's app slot.
//!
//! U0.1 scaffold: a plain `cortex-m-rt` entry that parks, proving the app
//! slot's `memory.x` links. U1.6 replaces the body with the real Embassy
//! blinky + defmt version banner + VBTBKR CAN-entry trigger.

#![no_std]
#![no_main]

use defmt_rtt as _; // defmt transport over RTT
use panic_probe as _; // panic = defmt log, then reset
use ra4m1_embassy as _; // __INTERRUPTS vector table (cortex-m-rt `device` feature)

use cortex_m_rt::entry;

#[entry]
fn main() -> ! {
    defmt::info!("demo-app scaffold up (U0.1) — blinky not yet implemented");

    loop {
        cortex_m::asm::wfi();
    }
}
