//! ADC14 (14-bit SAR ADC) — **stub** (M3.4, plan §4.2/§4.3).
//!
//! # What this phase implements
//!
//! A **bespoke inherent** API: blocking one-shot `read()` and async
//! `scan()`/`read()` awaiting the scan-complete IRQ via a waker
//! ([`crate::bind_interrupts`]). `embedded-hal` 1.0 has **no** stable ADC
//! trait (the 0.2 trait was dropped, plan §4.3/Q8), so no trait is implemented;
//! the inherent API is documented and stable, revisited if an ecosystem trait
//! stabilizes. Calibration / scaling to millivolts is pure, host-tested math.
//!
//! # PAC coverage (spike S-B)
//!
//! **Fully covered.** The PAC types `ADC140` over the `adc140` block
//! (ADCSR/ADANSA0..1/ADCER/ADSTRGR/ADDR[n]/ADSSTR.../compare-window regs). No
//! raw-MMIO seam needed. **Ready to implement** after the async pattern is
//! settled by M3.3.
//!
//! # Async model
//!
//! Async: scan-complete is a real hardware event, awaited via a waker set from
//! a minimal ISR — never a poll loop (rules/embassy.md).
