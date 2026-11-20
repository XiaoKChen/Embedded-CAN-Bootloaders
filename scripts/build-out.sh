#!/usr/bin/env bash
# Build the release firmware and export flashable .elf/.bin/.hex artifacts into out/.
#
# Builds the bootloader and the demo app for thumbv7em-none-eabihf, copies each ELF to out/,
# converts it to .bin and .hex, and prints the sizes against the 32768 B bootloader gate.
# Runs anywhere with the pinned Rust toolchain (Git Bash / WSL / Linux / CI).
#
# Errors:      `set -euo pipefail` — the first failing step aborts the script.
# Depends on:  llvm-objcopy / llvm-size from the toolchain's llvm-tools component (already
#              required by rust-toolchain.toml) — same tool CI uses.
# See also:    scripts/build-out.ps1 (the PowerShell 7+ port).
#
# Usage:  bash scripts/build-out.sh
set -euo pipefail

cd "$(dirname "$0")/.."

TARGET=thumbv7em-none-eabihf
OUT=out
HOST="$(rustc -vV | sed -n 's/host: //p')"
OBJCOPY="$(rustc --print sysroot)/lib/rustlib/$HOST/bin/llvm-objcopy"
SIZE="$(rustc --print sysroot)/lib/rustlib/$HOST/bin/llvm-size"

cargo build --release --target "$TARGET" -p bootloader -p demo-app

mkdir -p "$OUT"
for crate in bootloader demo-app; do
  elf="target/$TARGET/release/$crate"
  cp "$elf" "$OUT/$crate.elf"
  "$OBJCOPY" -O binary "$elf" "$OUT/$crate.bin"
  "$OBJCOPY" -O ihex   "$elf" "$OUT/$crate.hex"
done

echo
echo "Artifacts written to $OUT/:"
ls -l "$OUT"/bootloader.* "$OUT"/demo-app.*
echo
echo "Sizes (bootloader gate = 32768 B):"
"$SIZE" target/"$TARGET"/release/bootloader target/"$TARGET"/release/demo-app
