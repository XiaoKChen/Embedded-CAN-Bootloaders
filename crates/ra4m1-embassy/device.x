/* ==========================================================================
 * device.x — ra4m1-embassy (U0.3, spike S2)
 *
 * Weak aliases for the RA4M1's 32 ICU NVIC lines (IELSR0..31), consumed by
 * cortex-m-rt 0.7's link.x when its `device` feature is enabled (this crate
 * enables it — see Cargo.toml). Pairs with the hand-written `__INTERRUPTS`
 * table in src/time_driver.rs. The `ra4m1` PAC ships an identical file
 * behind its `rt` feature, which stays OFF (it drags in the conflicting
 * cortex-m-rt 0.6 runtime).
 *
 * IEL0 is strongly defined by the time driver (the project's only
 * interrupt); every other line falls back to cortex-m-rt's DefaultHandler.
 * ========================================================================== */
PROVIDE(IEL0 = DefaultHandler);
PROVIDE(IEL1 = DefaultHandler);
PROVIDE(IEL2 = DefaultHandler);
PROVIDE(IEL3 = DefaultHandler);
PROVIDE(IEL4 = DefaultHandler);
PROVIDE(IEL5 = DefaultHandler);
PROVIDE(IEL6 = DefaultHandler);
PROVIDE(IEL7 = DefaultHandler);
PROVIDE(IEL8 = DefaultHandler);
PROVIDE(IEL9 = DefaultHandler);
PROVIDE(IEL10 = DefaultHandler);
PROVIDE(IEL11 = DefaultHandler);
PROVIDE(IEL12 = DefaultHandler);
PROVIDE(IEL13 = DefaultHandler);
PROVIDE(IEL14 = DefaultHandler);
PROVIDE(IEL15 = DefaultHandler);
PROVIDE(IEL16 = DefaultHandler);
PROVIDE(IEL17 = DefaultHandler);
PROVIDE(IEL18 = DefaultHandler);
PROVIDE(IEL19 = DefaultHandler);
PROVIDE(IEL20 = DefaultHandler);
PROVIDE(IEL21 = DefaultHandler);
PROVIDE(IEL22 = DefaultHandler);
PROVIDE(IEL23 = DefaultHandler);
PROVIDE(IEL24 = DefaultHandler);
PROVIDE(IEL25 = DefaultHandler);
PROVIDE(IEL26 = DefaultHandler);
PROVIDE(IEL27 = DefaultHandler);
PROVIDE(IEL28 = DefaultHandler);
PROVIDE(IEL29 = DefaultHandler);
PROVIDE(IEL30 = DefaultHandler);
PROVIDE(IEL31 = DefaultHandler);
