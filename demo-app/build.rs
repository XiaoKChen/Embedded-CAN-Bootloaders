//! Linker-script plumbing for `memory.x` (same mechanism as
//! `bootloader/build.rs`) — not in the original U0.1 claimed-file list but
//! mechanically required for `memory.x` to reach the linker via `link.x`'s
//! `INCLUDE memory.x`; added alongside it.

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
