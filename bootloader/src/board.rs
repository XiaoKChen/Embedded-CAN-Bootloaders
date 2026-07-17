//! Board seam — the one module that names concrete chip peripherals
//! (shape per `.claude/template/embassy/board.rs`; no Embassy HAL exists
//! for the RA4M1, so the "HAL" here is `crates/ra4m1-embassy` over the raw
//! PAC singletons).
//!
//! `init()` acquires the PAC peripherals exactly once, brings the clock
//! tree up, and splits what `main` and tasks need into [`Board`]. Later
//! units extend `Board` with the peripherals their tasks own (CAN0, GPT,
//! USBFS, ...).

use ra4m1_embassy::{clocks, pfs};

/// UNO R4 Minima built-in LED "L": D13 = P111 (SPEC §2.5 drives it with
/// GPT ch3 PWM in parked mode; the spike just switches it on).
const LED_PORT: u8 = 1;
const LED_PIN: u8 = 11;

/// Everything `main` needs after bring-up, split once at boot.
pub struct Board {
    /// SYSTEM block: VBTBKR handoff words, reset/clock status registers.
    pub system: ra4m1::SYSTEM,
}

/// Bring the board up: clocks to HOCO 48 MHz, LED on as the visible
/// bring-up indicator. Call exactly once from `main`.
pub fn init() -> Board {
    // SAFETY: sole acquisition of the PAC singletons — called exactly once,
    // from `#[entry]`, before any other peripheral access exists. (The
    // checked `Peripherals::take()` is not compiled in because the PAC's
    // `critical-section` feature is disabled — Cargo.toml is frozen per
    // docs/plan.md §6.)
    let p = unsafe { ra4m1::Peripherals::steal() };

    clocks::init_hoco_48mhz(&p.SYSTEM);

    // LED on = S0 "board alive" indicator (the parked-mode fade is U1.5).
    pfs::set_gpio_output(&p.PFS, &p.PMISC, LED_PORT, LED_PIN, true);

    Board { system: p.SYSTEM }
}
