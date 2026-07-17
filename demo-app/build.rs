//! Linker-script plumbing for `memory.x` (same mechanism as
//! `bootloader/build.rs`) — not in the original U0.1 claimed-file list but
//! mechanically required for `memory.x` to reach the linker via `link.x`'s
//! `INCLUDE memory.x`; added alongside it.
//!
//! U1.6 also emits `DEMO_APP_BUILT`, the UTC build timestamp baked into the
//! defmt version banner (src/main.rs).

use std::env;
use std::fs::File;
use std::io::Write;
use std::path::PathBuf;
use std::time::{SystemTime, UNIX_EPOCH};

fn main() {
    let out = PathBuf::from(env::var_os("OUT_DIR").expect("OUT_DIR set by cargo"));
    File::create(out.join("memory.x"))
        .expect("create OUT_DIR/memory.x")
        .write_all(include_bytes!("memory.x"))
        .expect("write OUT_DIR/memory.x");

    println!("cargo:rustc-link-search={}", out.display());
    println!("cargo:rerun-if-changed=memory.x");
    println!("cargo:rerun-if-changed=build.rs");

    // Banner timestamp. Cached with this script's output: it refreshes on a
    // clean build (CI/release artifacts always get a fresh one) or when
    // build.rs/memory.x change — not on every incremental rebuild.
    println!("cargo:rustc-env=DEMO_APP_BUILT={}", utc_now());
}

/// Current UTC time as `YYYY-MM-DD HH:MM UTC`, chrono-free. Date via the
/// standard civil-from-days algorithm (Howard Hinnant, "chrono-Compatible
/// Low-Level Date Algorithms").
fn utc_now() -> String {
    let secs = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .expect("system clock set before 1970")
        .as_secs();
    let (hh, mm) = ((secs / 3600) % 24, (secs / 60) % 60);
    let days = secs / 86_400;
    let z = days + 719_468;
    let era = z / 146_097;
    let doe = z % 146_097;
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let day = doy - (153 * mp + 2) / 5 + 1;
    let month = if mp < 10 { mp + 3 } else { mp - 9 };
    let year = yoe + era * 400 + u64::from(month <= 2);
    format!("{year:04}-{month:02}-{day:02} {hh:02}:{mm:02} UTC")
}
