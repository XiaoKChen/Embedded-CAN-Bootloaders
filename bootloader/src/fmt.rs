//! Logging shim over `defmt`, compiled out when the `defmt` feature is off.
//!
//! Each macro forwards to the matching `defmt` macro when the feature is on, and compiles to a
//! no-op that still consumes its arguments when it is off. The rest of the crate logs with plain
//! `info!`/`warn!`/… call sites without a hard dependency on `defmt`, so the lean (default)
//! bootloader builds with `defmt` fully absent from the graph — it must stay linkable without
//! it for the 32 KiB flash budget (rules/rust.md: logging is otherwise `defmt`). Mirrors
//! `ra4m1-embassy`'s shim.
//!
//! Owns:        the `trace!`/`debug!`/`info!`/`warn!`/`error!` macros used crate-wide.
//! Invariants:  with the feature off, arguments are still evaluated by reference so call sites
//!              produce no unused-variable warnings.
//! See also:    `Cargo.toml` (`defmt` feature); `build.rs` (`-Tdefmt.x`).

#![allow(unused_macros)]

macro_rules! trace {
    ($s:literal $(, $x:expr)* $(,)?) => {{
        #[cfg(feature = "defmt")]
        ::defmt::trace!($s $(, $x)*);
        #[cfg(not(feature = "defmt"))]
        let _ = ($( & $x ),*);
    }};
}

macro_rules! debug {
    ($s:literal $(, $x:expr)* $(,)?) => {{
        #[cfg(feature = "defmt")]
        ::defmt::debug!($s $(, $x)*);
        #[cfg(not(feature = "defmt"))]
        let _ = ($( & $x ),*);
    }};
}

macro_rules! info {
    ($s:literal $(, $x:expr)* $(,)?) => {{
        #[cfg(feature = "defmt")]
        ::defmt::info!($s $(, $x)*);
        #[cfg(not(feature = "defmt"))]
        let _ = ($( & $x ),*);
    }};
}

macro_rules! warn {
    ($s:literal $(, $x:expr)* $(,)?) => {{
        #[cfg(feature = "defmt")]
        ::defmt::warn!($s $(, $x)*);
        #[cfg(not(feature = "defmt"))]
        let _ = ($( & $x ),*);
    }};
}

macro_rules! error {
    ($s:literal $(, $x:expr)* $(,)?) => {{
        #[cfg(feature = "defmt")]
        ::defmt::error!($s $(, $x)*);
        #[cfg(not(feature = "defmt"))]
        let _ = ($( & $x ),*);
    }};
}
