//! SPI bus — **stub** (M3.3, plan §4.2/§4.3).
//!
//! # What this phase implements
//!
//! The dedicated SPI block as blocking `embedded-hal::spi::SpiBus`
//! (+ `ErrorType`) and async `embedded-hal-async::spi::SpiBus`, with
//! `SpiDevice` (CS + bus sharing) provided via `embedded-hal-bus`. Clock
//! divider (SPBR, off `PCLKA` — [`crate::clock::Clock::PclkA`]) is pure,
//! host-tested math. Transfer uses the shared interrupt/DTC + waker pattern
//! proven by spike S-C.
//!
//! # PAC coverage (spike S-B)
//!
//! **Fully covered.** The PAC types `SPI0`/`SPI1` over the `spi0`/`spi1`
//! blocks (SPCR/SPSR/SPDR/SPBR/SPCMD0/SPDCR/...). No raw-MMIO seam needed.
//! **Ready to implement** after U3.3.1 (SCI) proves the shared async-transfer
//! pattern.
