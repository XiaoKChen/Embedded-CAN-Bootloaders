//! I2C bus (RIIC / IIC) — **stub** (M3.3, plan §4.2/§4.3).
//!
//! # What this phase implements
//!
//! The dedicated IIC block as blocking `embedded-hal::i2c::I2c` (+ `ErrorType`)
//! and async `embedded-hal-async::i2c::I2c`, with `I2cDevice` (bus sharing) via
//! `embedded-hal-bus`. Bit-rate registers (ICBRL/ICBRH, off `PCLKB` —
//! [`crate::clock::Clock::PclkB`]) and 7/10-bit address framing are pure,
//! host-tested logic. Transfer uses the shared interrupt + waker pattern
//! (spike S-C).
//!
//! # PAC coverage (spike S-B)
//!
//! **Fully covered.** The PAC types `IIC0`/`IIC1` over the `iic0`/`iic1`
//! blocks (ICCR1/ICCR2/ICMR1..3/ICSR1/2/ICBRL/ICBRH/ICDRT/ICDRR/SARL/SARU/...).
//! No raw-MMIO seam needed — the dedicated IIC peripheral is present, so the
//! SCI-simple-I2C fallback (plan §4.6) is unnecessary. **Ready to implement**
//! after U3.3.1 (SCI) proves the shared async-transfer pattern.
