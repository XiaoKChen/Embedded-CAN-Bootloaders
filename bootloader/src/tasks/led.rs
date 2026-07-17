//! Parked-mode LED fade: GPT channel 3 (GPT163) saw-wave PWM on
//! P111/GTIOC3A, triangle duty stepped every 100 ms (SPEC §2.5, U1.5).
//!
//! Spawned ONLY in parked mode: the C bootloader's CAN-only path
//! (`run_can_update_mode`, SPEC §2.3) never touched the LED — only the
//! parked loop ran `led_blinking_task` (SPEC §2.5). `main.rs` mirrors that
//! split.
//!
//! # C parity evidence
//!
//! The vendored C fork's sources are not checked out in this tree, so the
//! hardware configuration was recovered from the shipped, bench-proven C
//! binary (`vendor/arduino-renesas-bootloader/_build/uno_r4/*.bin` +
//! `.elf.map`):
//!
//! - `.rodata.pwm_cfg` (FSP `timer_cfg_t`): mode = PWM (saw), period
//!   `0x9896` counts, `source_div` = `TIMER_SOURCE_DIV_4` (PCLKD 48 MHz / 4
//!   = 12 MHz → ≈ 307 Hz PWM), initial duty `0x4C4B` (50 %), channel 3.
//! - `.rodata.g_timer_extend` (`gpt_extended_cfg_t`): GTIOCA output
//!   enabled, GTIOCB disabled, stop level low (`LED_FADE_PWM_OUT_A=1`,
//!   Makefile.minima).
//! - `.rodata.extra_pin_cfg`: `{0x0301_0000, P111}` = PMR set + PSEL 0x03
//!   (`IOPORT_PERIPHERAL_GPT1`, FSP `r_ioport_api.h`) — P111 → GTIOC3A.
//! - Fade stepping: `FADESTEP 2000` per 100 ms (SPEC §2.5).
//!
//! Register semantics: R01UM0007 (RA4M1 HW manual), GPT chapter, same
//! bring-up shape as `ra4m1_embassy::time_driver` (GTWP → GTCR → GTUDDTYC →
//! compare setup → CST), plus FSP `r_gpt.c` for the saw-PWM GTIOR encoding
//! and the buffered-compare duty semantics.

use embassy_time::{Duration, Ticker};
use ra4m1_embassy::pfs;

/// PWM period in GPT counts — C parity (`pwm_cfg.period_counts = 0x9896`,
/// ≈ 307 Hz at 12 MHz count clock).
const FADE_PERIOD_COUNTS: u32 = 0x9896;
/// Duty step per fade tick — C parity (`FADESTEP 2000`): a full ramp is
/// ~19 steps ≈ 1.9 s each way.
const FADE_STEP_COUNTS: u32 = 2000;
/// Initial duty — C parity (`pwm_cfg.duty_cycle_counts = 0x4C4B`, 50 %).
const FADE_INITIAL_DUTY_COUNTS: u32 = 0x4C4B;
/// Fade step cadence (SPEC §2.5: 100 ms step).
const FADE_STEP_PERIOD: Duration = Duration::from_millis(100);

/// PFS PSEL code routing P111 to GTIOC3A: 0x03 = the "GPT1" pin group
/// (module docs: recovered from the C binary's pin table; FSP
/// `IOPORT_PERIPHERAL_GPT1`).
const PSEL_GPT1: u8 = 0x03;
/// UNO R4 Minima built-in LED "L": D13 = P111 (SPEC §2.5).
const LED_PORT: u8 = 1;
const LED_PIN: u8 = 11;

/// Triangle-fade the LED forever. Owns GPT163 and the PFS/PMISC singletons
/// outright (narrowest primitive: move); `drivers::can0` touches only
/// P102/P103 through its own documented transient PFS access, and neither
/// task yields mid-PFS-sequence, so the PWPR protocol never interleaves.
#[embassy_executor::task]
pub async fn fade(gpt: ra4m1::GPT163, pfs_reg: ra4m1::PFS, pmisc: ra4m1::PMISC) {
    init_pwm(&gpt, &pfs_reg, &pmisc);

    let mut duty = FADE_INITIAL_DUTY_COUNTS;
    let mut rising = true;
    let mut ticker = Ticker::every(FADE_STEP_PERIOD);
    loop {
        ticker.next().await;
        if rising {
            duty += FADE_STEP_COUNTS;
            if duty >= FADE_PERIOD_COUNTS {
                duty = FADE_PERIOD_COUNTS;
                rising = false;
            }
        } else {
            duty -= FADE_STEP_COUNTS;
            if duty <= FADE_STEP_COUNTS {
                duty = FADE_STEP_COUNTS;
                rising = true;
            }
        }
        // Buffered compare: GTCCRC transfers into GTCCRA at cycle end
        // (GTBER.CCRA = 01), so mid-cycle updates are glitch-free. The −1
        // matches FSP's saw-PWM semantics (the pin transitions one count
        // after the compare match). Duty is clamped to
        // [FADE_STEP_COUNTS, FADE_PERIOD_COUNTS], so FSP's 0 %/100 %
        // pin-forcing special cases are never needed: the extremes render
        // as ~5 % (visibly dim) and ~100 % (one count low per cycle).
        // SAFETY: plain compare value; duty − 1 ≤ GTPR (R01UM0007, GTCCRn).
        gpt.gtccrc.write(|w| unsafe { w.bits(duty - 1) });
    }
}

/// Bring GPT163 up as a saw-wave PWM on GTIOC3A (module docs for the C
/// parity of every value). Pin routing happens last, so P111 never shows an
/// unconfigured timer output; this overrides `board::init`'s LED-on GPIO
/// state — PWM owns the pin from here.
fn init_pwm(gpt: &ra4m1::GPT163, pfs_reg: &ra4m1::PFS, pmisc: &ra4m1::PMISC) {
    // SAFETY: transient PAC re-acquisition for MSTPCRD's MSTPD6 bit only
    // (the 16-bit GPT ch2–7 group on this MCU — FSP `bsp_module_stop.h` +
    // `BSP_FEATURE_BSP_MSTP_GPT_MSTPD5_MAX_CH = 1`). The read-modify-write
    // cannot race: MSTPCRD's only other user (`time_driver::init`, MSTPD5)
    // ran pre-executor, and no ISR touches it.
    let p = unsafe { ra4m1::Peripherals::steal() };
    p.MSTP.mstpcrd.modify(|_, w| w.mstpd6()._0());
    let _ = p.MSTP.mstpcrd.read(); // completion read-back before GPT access

    // Write-enable the channel's registers (GTWP key 0xA5, WP = 0).
    gpt.gtwp.write(|w| w.prkey()._0x_a5().wp()._0());
    // Counter stopped while configuring: saw-wave mode (MD = 000), count
    // clock PCLKD/4 (TPCS = 001 → 12 MHz — C parity, TIMER_SOURCE_DIV_4).
    gpt.gtcr.write(|w| w.md()._000().tpcs()._001());
    // Count up, force-latched while stopped (UD alone is only sampled at
    // overflow; UDF pulses it through — same idiom as time_driver.rs).
    gpt.gtuddtyc.write(|w| w.ud()._1().udf()._1());
    gpt.gtuddtyc.write(|w| w.ud()._1().udf()._0());
    // GTIOCA = 01001: initial low, LOW at GTCCRA compare match, HIGH at
    // cycle end (FSP saw-PWM encoding `GPT_PRV_GTIO_LOW_COMPARE_MATCH_
    // HIGH_CYCLE_END = 0x9`); OAE enables the pin output. GTIOCB stays
    // disabled (C parity: LED_FADE_PWM_OUT_B=0).
    gpt.gtior.write(|w| w.gtioa()._01001().oae()._1());
    // Single-buffered GTCCRA: duty writes go to GTCCRC and transfer at
    // cycle end.
    gpt.gtber.write(|w| w.ccra()._01());
    // SAFETY: whole-register writes of plain, in-range count values
    // (R01UM0007, GPT chapter): GTPR/GTPBR hold period − 1; count from
    // zero; initial duty per the C config (compare value = duty − 1).
    unsafe {
        gpt.gtpr.write(|w| w.bits(FADE_PERIOD_COUNTS - 1));
        gpt.gtpbr.write(|w| w.bits(FADE_PERIOD_COUNTS - 1));
        gpt.gtcnt.write(|w| w.bits(0));
        gpt.gtccra.write(|w| w.bits(FADE_INITIAL_DUTY_COUNTS - 1));
        gpt.gtccrc.write(|w| w.bits(FADE_INITIAL_DUTY_COUNTS - 1));
    }

    // Route P111 → GTIOC3A (PSEL before PMR, handled by pfs.rs) and start.
    pfs::set_peripheral_function(pfs_reg, pmisc, LED_PORT, LED_PIN, PSEL_GPT1);
    gpt.gtcr.modify(|_, w| w.cst()._1());

    defmt::info!(
        "led: GPT ch3 PWM fade on P111 ({=u32} counts @ 12 MHz, 100 ms step)",
        FADE_PERIOD_COUNTS
    );
}
