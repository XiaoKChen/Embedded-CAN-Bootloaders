//! Linker-script plumbing for the HAL `examples/`.
//!
//! Copies `memory.x` into `OUT_DIR` and puts it on the linker search path so cortex-m-rt's `link.x`
//! can `INCLUDE memory.x`; same mechanism as `demo-app/build.rs`. Only the example binaries link;
//! the library itself needs no linker script.
//!
//! Owns:        the `memory.x` handoff to the linker, and the `-Tdefmt.x` link argument for
//!              examples.
//! Not here:    the memory map itself (`memory.x`); the target and runner (workspace
//!              `.cargo/config.toml`).
//! Invariants:  the workspace `.cargo/config.toml` no longer passes `-Tdefmt.x` (the lean
//!              bootloader has no `defmt.x`), so this script adds it, for examples only.
//! See also:    `memory.x`; `demo-app/build.rs`.

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
    println!("cargo:rerun-if-changed=memory.x");
    println!("cargo:rerun-if-changed=build.rs");
    // The workspace `.cargo/config.toml` no longer passes `-Tdefmt.x` (the lean
    // bootloader has no `defmt.x` to link); the examples always log over defmt.
    println!("cargo:rustc-link-arg-examples=-Tdefmt.x");
}
