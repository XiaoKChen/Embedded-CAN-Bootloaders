//! Volatile reads of code flash at address 0, via raw pointers — never
//! references (SPEC §9.1.7: a reference to address 0 is UB in Rust).
//!
//! Filled by U1.1: implements `boot_logic::image_meta`'s `ImageRead` trait.
