//! Build script: linker-script plumbing for the interrupt-vector aliases.
//!
//! Copies `device.x` (ICU interrupt-vector aliases, see src/time_driver.rs) into `OUT_DIR` and
//! adds it to the linker search path so cortex-m-rt's `link.x` — built with the `device`
//! feature — can `INCLUDE` it.
//!
//! Owns:      the `device.x` hand-off to the linker.
//! Errors:    panics (failing the build) when `OUT_DIR` is missing or the copy fails.
//! See also:  `device.x`, `src/time_driver.rs`.

use std::env;
use std::fs::File;
use std::io::Write;
use std::path::PathBuf;

fn main() {
    let out = PathBuf::from(env::var_os("OUT_DIR").expect("OUT_DIR set by cargo"));
    File::create(out.join("device.x"))
        .expect("create OUT_DIR/device.x")
        .write_all(include_bytes!("device.x"))
        .expect("write OUT_DIR/device.x");

    println!("cargo:rustc-link-search={}", out.display());
    println!("cargo:rerun-if-changed=device.x");
    println!("cargo:rerun-if-changed=build.rs");
}
