//! DAC12 (12-bit DAC) — **stub** (M3.4, plan §4.2/§4.3/§4.5).
//!
//! # What this phase implements
//!
//! A **bespoke inherent blocking** API: `set_value(u12)` is a single register
//! write with no completion event to await, so it stays blocking (plan §4.5 —
//! making it async would be theatre). `embedded-hal` has no DAC trait, so no
//! trait is implemented. Any output-voltage scaling is pure, host-tested math.
//!
//! # PAC coverage (spike S-B)
//!
//! **Fully covered.** The PAC types `DAC12` over the `dac12` block
//! (DADR0/DACR/DADPR/DAADSCR/DAVREFCR). No raw-MMIO seam needed. **Ready to
//! implement.**
