//! SCI serial — UART mode — **stub** (M3.3, plan §4.2/§4.5).
//!
//! # What this phase implements
//!
//! SCI in UART (async serial) mode: blocking `embedded-io::{Read, Write}` +
//! async `embedded-io-async::{Read, Write}`, RX/TX driven by an interrupt (or
//! the DTC — spike S-C decides) + waker via [`crate::bind_interrupts`]. The
//! baud divisor (BRR/SEMR/MDDR, sourced off `PCLKB` — [`crate::clock::Clock::PclkB`])
//! is a pure, host-tested function. `SCI-SPI` / `SCI-I2C` simple modes only if
//! a board needs them.
//!
//! # PAC coverage (spike S-B)
//!
//! **Fully covered.** The PAC types `SCI0`/`SCI1`/`SCI2`/`SCI9` over the
//! `sci0`/`sci2` blocks, including the FIFO variants (SMR, BRR, SCR, TDR, SSR,
//! RDR, FTDRHL, FRDRHL, FCR, FDR, and SEMR/MDDR for the baud fine-adjust). No
//! raw-MMIO seam needed. **Ready to implement** once spike S-A (interrupt
//! binding, done — see [`crate::interrupt`]) and S-C (transfer path: per-byte
//! IRQ vs DTC) land.
//!
//! # Async model
//!
//! Genuinely async: RX-ready / TX-complete are real hardware events, awaited
//! via a waker set from a minimal ISR (the `time_driver`/`usbfs` ISR shape) —
//! never a poll loop (rules/embassy.md).
