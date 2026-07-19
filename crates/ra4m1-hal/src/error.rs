//! Error convention for the HAL.
//!
//! Per rules/rust.md ("one error enum per module or subsystem") each
//! peripheral module owns its own error enum, `#[derive(Debug, defmt::Format)]`,
//! mapping to the relevant `embedded-hal` `ErrorKind` where the trait requires
//! it. There is deliberately **no** single crate-wide error enum: a UART
//! framing error and an I2C NACK are unrelated failure domains, and a wide
//! shared enum would violate "types are narrow at boundaries"
//! (rules/principles.md).
//!
//! Peripherals whose operations genuinely cannot fail on this hardware use
//! [`core::convert::Infallible`] as their trait `Error` type rather than an
//! empty enum — that is the honest type, and `embedded-hal` implements its
//! `Error`/`ErrorKind` traits for `Infallible` already. [`crate::gpio`] does
//! this for its blocking output/input paths: a PORT register write cannot
//! fail. (The async `digital::Wait` path, a later phase, may introduce a real
//! error type for a lost pin event; it will live in `gpio.rs`.)
//!
//! This module intentionally holds no types today. It exists so the
//! convention has a documented home and so a genuinely cross-cutting error
//! (should one ever be needed) has one obvious place to live.
