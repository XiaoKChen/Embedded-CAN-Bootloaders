#!/usr/bin/env bash
# Fetch the .gitignore'd vendored deps (CMSIS_5 + FSP) and build the
# UNO R4 Minima DFU+CAN bootloader on a Linux host. Mirrors docs/build.md.
#
# Requires arm-none-eabi-gcc (xPack 14.2.1) and GNU make already on PATH.
# The Makefile.minima size gate fails the build if the .bin exceeds the
# 16,384-byte bootloader region.
#
# Usage: scripts/ci-build.sh    (run from the repo root)
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TINYUSB="$REPO_ROOT/vendor/tinyusb"
FORK="$REPO_ROOT/vendor/arduino-renesas-bootloader"

# Pinned dep commits (see docs/build.md "Vendored components").
FSP_COMMIT="d52e5a6a59b7c638da860c2bb309b6e78e752ff8"
CMSIS_COMMIT="20285262657d1b482d132d20d755c8c330d55c1f"

echo "== toolchain =="
arm-none-eabi-gcc --version | head -1

cd "$TINYUSB"
if [ ! -e hw/mcu/renesas/fsp/ra/fsp/src/r_flash_lp/r_flash_lp.c ]; then
  echo "== fetching renesas/fsp @ $FSP_COMMIT =="
  mkdir -p hw/mcu/renesas/fsp
  git -C hw/mcu/renesas/fsp init
  git -C hw/mcu/renesas/fsp fetch --depth 1 https://github.com/renesas/fsp.git "$FSP_COMMIT"
  git -C hw/mcu/renesas/fsp checkout FETCH_HEAD
fi
if [ ! -e lib/CMSIS_5/CMSIS/Core/Include/core_cm4.h ]; then
  echo "== fetching ARM-software/CMSIS_5 @ $CMSIS_COMMIT =="
  mkdir -p lib/CMSIS_5
  git -C lib/CMSIS_5 init
  git -C lib/CMSIS_5 fetch --depth 1 https://github.com/ARM-software/CMSIS_5.git "$CMSIS_COMMIT"
  git -C lib/CMSIS_5 checkout FETCH_HEAD
fi

echo "== build =="
cd "$FORK"
rm -rf _build
make -f Makefile.minima BOARD=uno_r4 TINYUSB_ROOT=../tinyusb \
     CAN_LOADER=1 DFU_CUSTOM_NAME=1 -j"$(nproc)"

echo "== artifacts =="
ls -la _build/uno_r4/arduino-renesas-bootloader.elf \
       _build/uno_r4/arduino-renesas-bootloader.bin \
       _build/uno_r4/arduino-renesas-bootloader.hex
