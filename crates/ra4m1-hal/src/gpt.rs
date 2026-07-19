//! Timers / PWM on the GPT block — **stub** (M3.2, plan §4.2/§4.4).
//!
//! # What this phase implements
//!
//! GPT channel-ownership tokens for channels 1..=7, blocking PWM via
//! `embedded-hal` `pwm::SetDutyCycle` (+ `pwm::ErrorType`) on GTIOCn pins
//! routed through [`crate::pfs::set_peripheral_function`], and an optional
//! inherent `async fn capture()` (no standard trait for capture). Period/duty
//! arithmetic is pure, host-tested math.
//!
//! # Channel map (spike S-B, confirmed against the `ra4m1` 0.2 PAC)
//!
//! Eight GPT channels exist: two 32-bit (`GPT320`, `GPT321`) and six 16-bit
//! (`GPT162`..`GPT167`), i.e. channels 0..=7.
//!
//! | Channel | PAC singleton | Width  | Availability |
//! |---------|---------------|--------|--------------|
//! | 0 | `GPT320` | 32-bit | **reserved** — the `embassy-time` driver (plan §1.3). Never a HAL PWM channel. |
//! | 1 | `GPT321` | 32-bit | app PWM/capture |
//! | 2 | `GPT162` | 16-bit | app PWM/capture |
//! | 3 | `GPT163` | 16-bit | app PWM/capture (the bootloader LED-fade spike used this) |
//! | 4 | `GPT164` | 16-bit | app PWM/capture |
//! | 5 | `GPT165` | 16-bit | app PWM/capture |
//! | 6 | `GPT166` | 16-bit | app PWM/capture |
//! | 7 | `GPT167` | 16-bit | app PWM/capture |
//!
//! This resolves plan §9 Q7. The channel-ownership token must refuse channel 0
//! at the type level.
//!
//! # PAC coverage
//!
//! **Fully covered** — the base time driver already drives `GPT320` via the
//! PAC's typed `gpt320` block; `gpt162` is the shared layout for the 16-bit
//! channels. No raw-MMIO seam needed. **Ready to implement.**
