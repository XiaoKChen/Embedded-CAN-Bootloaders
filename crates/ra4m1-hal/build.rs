//! Linker-script plumbing for the HAL `examples/` (same mechanism as
//! `demo-app/build.rs`): copy `memory.x` into `OUT_DIR` and put it on the
//! linker search path so cortex-m-rt's `link.x` can `INCLUDE memory.x`. Only
//! the example binaries link; the library itself needs no linker script.

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
}
