//! Logging shim. Each macro forwards to the matching `defmt` macro when the
//! `defmt` feature is on, and compiles to a no-op that still consumes its
//! arguments when it is off. This lets the rest of the crate log with plain
//! `info!`/`warn!`/… call sites without a hard dependency on `defmt`, so the
//! lean (default) bootloader builds with `defmt` fully absent from the graph
//! (rules/rust.md: logging is `defmt`, but the bootloader must stay linkable
//! without it for the 32 KiB flash budget). Mirrors `ra4m1-embassy`'s shim.

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
