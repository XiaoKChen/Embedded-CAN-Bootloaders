//! Timers / PWM on the GPT block (M3.2, plan §4.2/§4.4).
//!
//! # What this module implements
//!
//! Blocking saw-wave (edge-aligned) PWM on GPT channels **1..=7**, one
//! [`Pwm`] per channel. Duty is glitch-free: writes land in the single-buffer
//! compare register and transfer into the active compare at cycle end
//! (`GTBER.CCRx = 01`), exactly the bench-proven recipe the bootloader LED-fade
//! uses (`bootloader/src/tasks/led.rs`, GPT163 on P111). The public duty seam is
//! `embedded-hal` 1.0 [`SetDutyCycle`](embedded_hal::pwm::SetDutyCycle);
//! full-resolution native control and frequency/period are inherent methods.
//!
//! # Channel map (spike S-B, `docs/research/ra4m1-pac-coverage.md`)
//!
//! Eight GPT channels exist: two 32-bit (`GPT320`, `GPT321`) and six 16-bit
//! (`GPT162`..`GPT167`), i.e. channels 0..=7.
//!
//! | Channel | PAC singleton | Width  | Availability |
//! |---------|---------------|--------|--------------|
//! | 0 | `GPT320` | 32-bit | **reserved** — the `embassy-time` driver (plan §1.3). Never a HAL PWM channel. |
//! | 1 | `GPT321` | 32-bit | app PWM ([`GptChannel`]) |
//! | 2 | `GPT162` | 16-bit | app PWM |
//! | 3 | `GPT163` | 16-bit | app PWM (the bootloader LED-fade spike used this — P111/GTIOC3A) |
//! | 4 | `GPT164` | 16-bit | app PWM |
//! | 5 | `GPT165` | 16-bit | app PWM |
//! | 6 | `GPT166` | 16-bit | app PWM |
//! | 7 | `GPT167` | 16-bit | app PWM |
//!
//! Channel 0 is refused **at the type level**: `GPT320` does not implement
//! [`GptChannel`], so it can never be handed to [`Pwm::new`].
//!
//! # Register access
//!
//! Every GPT channel's register block shares one layout: `GPT320` (ch0/1) and
//! `GPT162` (ch2..7) are byte-identical in the `ra4m1` 0.2 PAC and every
//! register is a 32-bit word (16-bit channels ignore the upper half of
//! `GTCNT`/`GTPR`/`GTCCRn`). This module therefore reaches a channel by its one
//! documented computed base — `0x4007_8000 + 0x100·channel` — and uses the
//! PAC's **typed** register accessors through the shared `gpt162` layout, the
//! same contained-address discipline as [`crate::gpio`] / `pfs.rs`. No raw
//! field pokes; the only `unsafe` are the computed-base reference and the
//! whole-word `bits()` writes of in-range count values.
//!
//! Register semantics: R01UM0007 (RA4M1 HW manual), GPT chapter — `GTWP`
//! (write-protect key `0xA5`), `GTCR` (`MD` saw mode, `TPCS` prescaler),
//! `GTUDDTYC` (`UD`/`UDF` count-up latch, `OADTY`/`OBDTY` 0 %/100 % force),
//! `GTIOR` (`GTIOA`/`GTIOB` I/O control + `OAE`/`OBE` output enable), `GTBER`
//! (`CCRA`/`CCRB` buffer enable), `GTPR`/`GTPBR` (period), `GTCCRA`/`GTCCRC`
//! (compare A + its buffer), `GTCCRB`/`GTCCRD` (compare B + its buffer), and
//! `MSTPCRD.MSTPD5`/`MSTPD6` (module-stop release) — cross-referenced against
//! the C-parity values in `bootloader/src/tasks/led.rs`.

use core::convert::Infallible;

use embedded_hal::pwm::{self, SetDutyCycle};
use ra4m1::{PFS, PMISC};

use crate::clock;
use crate::pfs;

/// GPT register-block base (R01UM0007, GPT register map). Channel `n` lives at
/// `GPT_BASE + GPT_STRIDE·n`.
const GPT_BASE: usize = 0x4007_8000;
/// Stride between consecutive GPT channel register blocks.
const GPT_STRIDE: usize = 0x100;

/// PFS PSEL code that routes a pin to its GPT (GTIOCn) function
/// (`IOPORT_PERIPHERAL_GPT` = 0b00011; recovered on silicon for P111→GTIOC3A in
/// `led.rs`). All GTIOC pins share this code.
const PSEL_GPT: u8 = 0x03;

/// GPT count-clock prescaler dividers, indexed by `GTCR.TPCS` (R01UM0007, GTCR
/// TPCS field: 0→÷1, 1→÷4, 2→÷16, 3→÷64, 4→÷256, 5→÷1024). Ascending, so the
/// first divider that fits the counter width gives the finest resolution.
const PRESCALERS: [u32; 6] = [1, 4, 16, 64, 256, 1024];

/// Errors configuring a GPT PWM channel.
///
/// Duty updates cannot fail (they are clamped register writes — the
/// [`SetDutyCycle`] error type is [`Infallible`]); only frequency selection can.
#[derive(Clone, Copy, Debug, PartialEq, Eq, defmt::Format)]
#[non_exhaustive]
pub enum Error {
    /// No prescaler produces the requested frequency within the channel's
    /// counter width at the current PCLKD. Raise the frequency, or use a 32-bit
    /// channel ([`GptChannel::WIDTH_BITS`] = 32) for very low frequencies.
    FrequencyUnreachable,
}

/// Which GTIOC output of the channel a [`Pwm`] drives.
///
/// A channel has two independent compare/output pairs; a [`Pwm`] owns one.
/// Output A uses compare `GTCCRA` (buffered through `GTCCRC`); output B uses
/// `GTCCRB` (buffered through `GTCCRD`).
#[derive(Clone, Copy, Debug, PartialEq, Eq, defmt::Format)]
pub enum PwmOutput {
    /// GTIOCnA.
    A,
    /// GTIOCnB.
    B,
}

mod sealed {
    pub trait Sealed {}
}

/// A GPT channel the HAL offers for application PWM (channels 1..=7).
///
/// Implemented only for the app-usable channel singletons; `GPT320` (channel 0,
/// the `embassy-time` driver, plan §1.3) deliberately does **not** implement it,
/// so channel 0 is excluded at the type level. Passing the channel singleton to
/// [`Pwm::new`] proves single ownership and moves it into the driver.
pub trait GptChannel: sealed::Sealed {
    /// Channel index (1..=7): `(base − 0x4007_8000) / 0x100`.
    const CHANNEL: u8;
    /// Counter width in bits: 32 for `GPT321`, 16 for `GPT162`..`GPT167`. Bounds
    /// the largest period ([`Pwm::max_duty_counts`]) the channel can represent.
    const WIDTH_BITS: u8;
}

macro_rules! impl_channel {
    ($ty:ident, $ch:expr, $width:expr) => {
        impl sealed::Sealed for ra4m1::$ty {}
        impl GptChannel for ra4m1::$ty {
            const CHANNEL: u8 = $ch;
            const WIDTH_BITS: u8 = $width;
        }
    };
}

// Channel 0 (GPT320) is intentionally omitted — reserved for the time driver.
impl_channel!(GPT321, 1, 32);
impl_channel!(GPT162, 2, 16);
impl_channel!(GPT163, 3, 16);
impl_channel!(GPT164, 4, 16);
impl_channel!(GPT165, 5, 16);
impl_channel!(GPT166, 6, 16);
impl_channel!(GPT167, 7, 16);

/// The GPT register block for channel `C`, via its documented computed base.
///
/// All GPT channels share the `gpt162` layout (identical to `gpt320` in the PAC,
/// verified byte-for-byte); only the semantic width of the count registers
/// differs, and every register is a 32-bit word regardless. Using the `gpt162`
/// layout for the 32-bit channel `GPT321` is therefore sound.
fn regs<C: GptChannel>() -> &'static ra4m1::gpt162::RegisterBlock {
    // SAFETY: address = documented GPT base + 0x100·channel, which lands on the
    // GPTn block for channel 1..=7 (R01UM0007, GPT register map). The layout is
    // identical across GPT320/GPT162 and all registers are 32-bit words. Shared-
    // reference MMIO is the PAC's own model; the owning `Pwm<C>` holds channel
    // `C`'s singleton, so no other typed access aliases this block.
    unsafe {
        &*((GPT_BASE + GPT_STRIDE * C::CHANNEL as usize) as *const ra4m1::gpt162::RegisterBlock)
    }
}

/// The largest `GTPR` value (period − 1) a counter of `width_bits` can hold.
const fn max_gtpr(width_bits: u8) -> u32 {
    if width_bits >= 32 {
        u32::MAX
    } else {
        (1u32 << width_bits) - 1
    }
}

/// Pick the finest-resolution `(TPCS index, period in counts)` that renders
/// `target_hz` within `width_bits` of counter at `pclkd_hz`, or `None` if no
/// prescaler fits.
///
/// Pure arithmetic (host-tested): the PWM period is `count_clk / target_hz`
/// where `count_clk = pclkd_hz / prescaler`. Smallest prescaler first, so the
/// first period that fits the width has the most steps.
fn compute_timing(pclkd_hz: u32, target_hz: u32, width_bits: u8) -> Option<(u8, u32)> {
    if target_hz == 0 {
        return None;
    }
    let limit = max_gtpr(width_bits);
    for (index, div) in PRESCALERS.into_iter().enumerate() {
        let count_clk = pclkd_hz / div;
        let period = count_clk / target_hz;
        // period ≥ 1 (target not above the count clock) and period − 1 ≤ GTPR max.
        if period >= 1 && period - 1 <= limit {
            // index is 0..=5, well within u8.
            return Some((index as u8, period));
        }
    }
    None
}

/// A single GPT channel driving edge-aligned PWM on one GTIOC output.
///
/// Owns its channel singleton (moved in at [`new`](Pwm::new)) — construct each
/// channel once and hand the value into the task that drives it. The output
/// starts **stopped**; call [`start`](Pwm::start).
///
/// Duty resolution equals the period in counts ([`max_duty_counts`](Pwm::max_duty_counts)),
/// which the `embedded-hal` [`SetDutyCycle`] (u16) seam caps at
/// [`u16::MAX`] and scales onto for 32-bit channels with very long periods.
pub struct Pwm<C: GptChannel> {
    /// Retained to enforce single ownership of the channel (register access goes
    /// through [`regs`] by `C::CHANNEL`).
    _channel: C,
    output: PwmOutput,
    /// Period in counts (== `GTPR + 1` == full-scale duty).
    period: u32,
    /// Current duty in counts (0..=`period`).
    duty: u32,
}

impl<C: GptChannel> Pwm<C> {
    /// Configure channel `C` for edge-aligned PWM on `Pport_pin` (a GTIOCn pin)
    /// at `frequency_hz`, initial duty 50 %, output stopped.
    ///
    /// `port`/`pin` are the GTIOC pin coordinates routed via
    /// [`pfs::set_peripheral_function`] (PSEL `0x03`). Known board mapping:
    /// P111 (port 1, pin 11 = UNO R4 Minima D13 "L" LED) → GTIOC3A, i.e.
    /// channel `GPT163` with [`PwmOutput::A`] (`led.rs`).
    ///
    /// # Errors
    ///
    /// [`Error::FrequencyUnreachable`] if no prescaler renders `frequency_hz`
    /// within the channel's counter width at PCLKD.
    pub fn new(
        channel: C,
        pfs_reg: &PFS,
        pmisc: &PMISC,
        port: u8,
        pin: u8,
        output: PwmOutput,
        frequency_hz: u32,
    ) -> Result<Self, Error> {
        let (tpcs, period) = compute_timing(clock::PCLKD_HZ, frequency_hz, C::WIDTH_BITS)
            .ok_or(Error::FrequencyUnreachable)?;

        release_mstp::<C>();
        let gpt = regs::<C>();

        // Write-enable this channel's registers (GTWP key 0xA5, WP = 0).
        gpt.gtwp.write(|w| w.prkey()._0x_a5().wp()._0());

        // Counter stopped while configuring: saw-wave (MD = 000) + prescaler.
        gpt.gtcr.write(|w| {
            let w = w.md()._000();
            match tpcs {
                0 => w.tpcs()._000(),
                1 => w.tpcs()._001(),
                2 => w.tpcs()._010(),
                3 => w.tpcs()._011(),
                4 => w.tpcs()._100(),
                _ => w.tpcs()._101(),
            }
        });

        // Count up, force-latched while stopped (UD is only sampled at overflow;
        // UDF pulses it through — the time_driver.rs/led.rs idiom).
        gpt.gtuddtyc.write(|w| w.ud()._1().udf()._1());
        gpt.gtuddtyc.write(|w| w.ud()._1().udf()._0());

        // GTIOCx = 01001: initial LOW, LOW at compare match, HIGH at cycle end
        // (FSP saw-PWM `GPT_PRV_GTIO_LOW_COMPARE_MATCH_HIGH_CYCLE_END`); enable
        // the chosen output. Single-buffer the active compare so runtime duty
        // writes (to GTCCRC/GTCCRD) transfer at cycle end — glitch-free.
        match output {
            PwmOutput::A => {
                gpt.gtior.write(|w| w.gtioa()._01001().oae()._1());
                gpt.gtber.write(|w| w.ccra()._01());
            }
            PwmOutput::B => {
                gpt.gtior.write(|w| w.gtiob()._01001().obe()._1());
                gpt.gtber.write(|w| w.ccrb()._01());
            }
        }

        // SAFETY: whole-register writes of plain, in-range count values
        // (R01UM0007, GPT chapter): GTPR/GTPBR hold period − 1; count from zero.
        unsafe {
            gpt.gtpr.write(|w| w.bits(period - 1));
            gpt.gtpbr.write(|w| w.bits(period - 1));
            gpt.gtcnt.write(|w| w.bits(0));
        }

        let mut me = Self {
            _channel: channel,
            output,
            period,
            duty: 0,
        };
        // Seed both the active compare and its buffer so the first cycle is
        // already at the initial duty.
        let initial = period / 2;
        me.write_compare(initial, true);
        me.duty = initial;

        // Route the pin last (PSEL before PMR, handled by pfs.rs), so it never
        // shows an unconfigured timer output.
        pfs::set_peripheral_function(pfs_reg, pmisc, port, pin, PSEL_GPT);

        Ok(me)
    }

    /// The counter width in bits (16 or 32), from [`GptChannel::WIDTH_BITS`].
    #[must_use]
    pub const fn width_bits(&self) -> u8 {
        C::WIDTH_BITS
    }

    /// The current period in counts (full-scale duty).
    #[must_use]
    pub const fn period_counts(&self) -> u32 {
        self.period
    }

    /// Full-scale duty in native counts (== [`period_counts`](Pwm::period_counts)).
    /// Unlike the u16 [`SetDutyCycle::max_duty_cycle`] seam, this exposes the
    /// channel's full resolution (up to 2³² for a 32-bit channel).
    #[must_use]
    pub const fn max_duty_counts(&self) -> u32 {
        self.period
    }

    /// Set the duty in native counts (0..=`period`), glitch-free. Values above
    /// the period saturate to 100 %.
    pub fn set_duty_counts(&mut self, duty: u32) {
        let duty = duty.min(self.period);
        self.duty = duty;
        self.write_compare(duty, false);
    }

    /// Re-target the PWM frequency, re-selecting the prescaler and period and
    /// re-applying the current duty proportionally.
    ///
    /// Best called while [`stopped`](Pwm::stop) — changing the prescaler on a
    /// running counter can glitch the current cycle. The new period/compare are
    /// buffered, so once running they take effect at the next cycle end.
    ///
    /// # Errors
    ///
    /// [`Error::FrequencyUnreachable`] if no prescaler fits (period unchanged).
    pub fn set_frequency(&mut self, frequency_hz: u32) -> Result<(), Error> {
        let (tpcs, period) = compute_timing(clock::PCLKD_HZ, frequency_hz, C::WIDTH_BITS)
            .ok_or(Error::FrequencyUnreachable)?;
        // Preserve the duty fraction across the period change.
        let duty = if self.period == 0 {
            0
        } else {
            u32::try_from(u64::from(self.duty) * u64::from(period) / u64::from(self.period))
                .unwrap_or(period)
        };

        let gpt = regs::<C>();
        gpt.gtcr.modify(|_, w| match tpcs {
            0 => w.tpcs()._000(),
            1 => w.tpcs()._001(),
            2 => w.tpcs()._010(),
            3 => w.tpcs()._011(),
            4 => w.tpcs()._100(),
            _ => w.tpcs()._101(),
        });
        // SAFETY: buffered period register write of an in-range value
        // (R01UM0007, GTPBR); transfers into GTPR at cycle end.
        unsafe {
            gpt.gtpbr.write(|w| w.bits(period - 1));
        }
        self.period = period;
        self.set_duty_counts(duty);
        Ok(())
    }

    /// Start the counter (`GTCR.CST = 1`); the output begins toggling.
    pub fn start(&mut self) {
        regs::<C>().gtcr.modify(|_, w| w.cst()._1());
    }

    /// Stop the counter (`GTCR.CST = 0`); the output holds its stop level (low).
    pub fn stop(&mut self) {
        regs::<C>().gtcr.modify(|_, w| w.cst()._0());
    }

    /// Apply `duty` counts to the active output. `duty == 0` and `duty >= period`
    /// use the `GTUDDTYC` 0 %/100 % duty-force (`OADTY`/`OBDTY`) so the extremes
    /// are exact — no one-count glitch a bare compare value would leave. In
    /// between, the force is cleared and the compare value (`duty − 1`) is
    /// written to the single-buffer register (`GTCCRC`/`GTCCRD`), transferring
    /// into the active compare at cycle end. When `seed`, the active compare
    /// (`GTCCRA`/`GTCCRB`) is written too so the very first cycle is correct.
    fn write_compare(&self, duty: u32, seed: bool) {
        let gpt = regs::<C>();
        if duty == 0 {
            match self.output {
                PwmOutput::A => gpt.gtuddtyc.modify(|_, w| w.oadty()._10()),
                PwmOutput::B => gpt.gtuddtyc.modify(|_, w| w.obdty()._10()),
            }
            return;
        }
        if duty >= self.period {
            match self.output {
                PwmOutput::A => gpt.gtuddtyc.modify(|_, w| w.oadty()._11()),
                PwmOutput::B => gpt.gtuddtyc.modify(|_, w| w.obdty()._11()),
            }
            return;
        }
        let compare = duty - 1;
        match self.output {
            PwmOutput::A => {
                gpt.gtuddtyc.modify(|_, w| w.oadty()._00());
                // SAFETY: compare = duty − 1 < period ≤ GTPR + 1, so it fits the
                // counter width (R01UM0007, GTCCR). Buffered via GTCCRC.
                unsafe {
                    gpt.gtccrc.write(|w| w.bits(compare));
                    if seed {
                        gpt.gtccra.write(|w| w.bits(compare));
                    }
                }
            }
            PwmOutput::B => {
                gpt.gtuddtyc.modify(|_, w| w.obdty()._00());
                // SAFETY: as above, buffered via GTCCRD into GTCCRB.
                unsafe {
                    gpt.gtccrd.write(|w| w.bits(compare));
                    if seed {
                        gpt.gtccrb.write(|w| w.bits(compare));
                    }
                }
            }
        }
    }
}

impl<C: GptChannel> pwm::ErrorType for Pwm<C> {
    /// A duty write is a clamped register write — it cannot fail. Frequency
    /// configuration (which can) is the inherent [`Pwm::set_frequency`], not this
    /// trait, and carries the real [`Error`].
    type Error = Infallible;
}

impl<C: GptChannel> SetDutyCycle for Pwm<C> {
    fn max_duty_cycle(&self) -> u16 {
        // The trait's duty is u16; native resolution can exceed it on a 32-bit
        // channel, so cap the reported scale at u16::MAX (set_duty_cycle scales).
        u16::try_from(self.period.min(u32::from(u16::MAX))).unwrap_or(u16::MAX)
    }

    fn set_duty_cycle(&mut self, duty: u16) -> Result<(), Infallible> {
        let max = self.max_duty_cycle();
        let counts = if max == 0 {
            0
        } else {
            // Scale the u16 request onto the native period. When period ≤ u16::MAX
            // (all 16-bit channels), max == period and this is an identity.
            u32::try_from(u64::from(duty) * u64::from(self.period) / u64::from(max))
                .unwrap_or(self.period)
        };
        self.set_duty_counts(counts);
        Ok(())
    }
}

/// Release channel `C` from module-stop before its registers are touched
/// (R01UM0007, Low Power Modes: `MSTPCRD.MSTPD5` gates GPT ch0/1, `MSTPD6` gates
/// ch2..7 — FSP `BSP_FEATURE_BSP_MSTP_GPT_MSTPD5_MAX_CH = 1`, as in `led.rs`).
fn release_mstp<C: GptChannel>() {
    // SAFETY: shared-reference MMIO to the MSTP block — the PAC's own model. The
    // read-modify-write cannot race: HAL driver `init`s run before the executor
    // starts, single-threaded, and no ISR touches MSTPCRD. MSTPD5 may already be
    // clear (the time driver released it for ch0); re-clearing is idempotent.
    let mstp = unsafe { &*ra4m1::MSTP::PTR };
    if C::CHANNEL <= 1 {
        mstp.mstpcrd.modify(|_, w| w.mstpd5()._0());
    } else {
        mstp.mstpcrd.modify(|_, w| w.mstpd6()._0());
    }
    let _ = mstp.mstpcrd.read(); // completion read-back before GPT access
}
