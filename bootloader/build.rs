//! Build script: linker-script plumbing for the bootloader image.
//!
//! Copies `memory.x` into `OUT_DIR` and adds it to the linker search path so `link.x` (from
//! `cortex-m-rt`, pulled in via `.cargo/config.toml`'s `-Tlink.x`) can `INCLUDE` it, and emits
//! `-Tdefmt.x` only when the `defmt` feature is on.
//!
//! Owns:      the link arguments that cannot live in `.cargo/config.toml` (the feature-gated
//!            `-Tdefmt.x`).
//! Not here:  the flash/RAM partition itself (`memory.x`).
//! See also:  `memory.x`; `Cargo.toml` (`defmt` feature).

use std::env;
use std::fs::File;
use std::io::Write;
use std::path::PathBuf;

fn main() {
    let out = PathBuf::from(env::var_os("OUT_DIR").expect("OUT_DIR set by cargo"));
    File::create(out.join("memory.x"))
        .expect("create OUT_DIR/memory.x")
        .write_all(include_bytes!("memory.x"))
        .expect("write OUT_DIR/memory.x");

    println!("cargo:rustc-link-search={}", out.display());

    // `-Tdefmt.x` (the defmt symbol table) is emitted here, gated on the
    // `defmt` feature, rather than unconditionally in `.cargo/config.toml`:
    // `defmt.x` is produced by the `defmt` crate's build script, so the lean
    // build (defmt absent from the graph) would fail to link with a
    // "cannot find defmt.x" error if the arg were always present. A cargo
    // config can't be feature-gated; a build script can (`CARGO_FEATURE_DEFMT`).
    if env::var_os("CARGO_FEATURE_DEFMT").is_some() {
        println!("cargo:rustc-link-arg=-Tdefmt.x");
    }

    println!("cargo:rerun-if-changed=memory.x");
    println!("cargo:rerun-if-changed=build.rs");
}
