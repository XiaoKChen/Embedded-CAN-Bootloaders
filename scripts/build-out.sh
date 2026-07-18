#!/usr/bin/env bash
# Build the release firmware and export flashable artifacts (.elf/.bin/.hex)
# for both the bootloader and the demo app into out/.
#
# Usage:  bash scripts/build-out.sh
# Runs anywhere with the pinned Rust toolchain (Git Bash / WSL / Linux / CI).
# .hex/.bin are produced with llvm-objcopy from the toolchain's llvm-tools
# component (already required by rust-toolchain.toml) — same tool CI uses.
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
