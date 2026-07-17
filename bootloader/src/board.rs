//! Board seam — the one module that names concrete chip peripherals
//! (shape per `.claude/template/embassy/board.rs`; no Embassy HAL exists
//! for the RA4M1, so the "HAL" here is `crates/ra4m1-embassy` over the raw
//! PAC singletons).
//!
//! `init()` acquires the PAC peripherals exactly once, brings the clock
//! tree up, and splits what `main` and the tasks need into [`Board`]:
//! `main` consumes `system` for the boot decision (VBTBKR handoff words,
//! reset-status read) and hands the GPT/PFS pieces to the parked-mode LED
//! task at spawn time. Peripherals the drivers bring up themselves (CAN0,
//! USBFS's rail registers, the flash sequencer) are acquired inside those
//! drivers under their own documented ownership rules.

use ra4m1_embassy::{clocks, pfs};

/// UNO R4 Minima built-in LED "L": D13 = P111. Switched on here as the
/// bring-up indicator; in parked mode `tasks::led` repurposes the pin for
/// the GPT ch3 PWM fade (SPEC §2.5).
const LED_PORT: u8 = 1;
const LED_PIN: u8 = 11;

/// Everything `main` needs after bring-up, split once at boot.
pub struct Board {
    /// SYSTEM block: VBTBKR handoff words, reset/clock status registers.
    pub system: ra4m1::SYSTEM,
    /// GPT channel 3 — the parked-mode LED PWM (`tasks::led`).
    pub gpt163: ra4m1::GPT163,
    /// Pin-function blocks, owned by the LED task after spawn. CAN0's
    /// driver uses its own transient, documented PFS access for P102/P103
    /// only (see `drivers::can0::init_hw`'s SAFETY note).
    pub pfs: ra4m1::PFS,
    pub pmisc: ra4m1::PMISC,
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

    // LED on = "board alive" indicator: stays solid in CAN-only mode,
    // replaced by the PWM fade once the parked-mode LED task starts, and
    // visible only for the microseconds before an app jump.
    pfs::set_gpio_output(&p.PFS, &p.PMISC, LED_PORT, LED_PIN, true);

    Board {
        system: p.SYSTEM,
        gpt163: p.GPT163,
        pfs: p.PFS,
        pmisc: p.PMISC,
    }
}
