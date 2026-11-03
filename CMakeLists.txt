# Root build for the UNO R4 Minima DFU+CAN bootloader repo.
#
# Two build surfaces from one tree:
#   Host tests:  cmake -B build-host && cmake --build build-host
#                && ctest --test-dir build-host --output-on-failure
#   Bootloader:  cmake --build build-host --target bootloader
#                (wraps the vendored fork's GNU Make / tinyusb build — NOT
#                ported to CMake; toolchain prerequisites in docs/build.md)
#
# Not here:    The host test targets (tests/CMakeLists.txt); the firmware
#              build rules (vendor/arduino-renesas-bootloader/Makefile.minima).
# See also:    tests/CMakeLists.txt, docs/build.md

cmake_minimum_required(VERSION 3.20)

project(arduino_bootloader_can LANGUAGES NONE)

enable_testing()

# -----------------------------------------------------------------------------
# Host-side Unity tests (pure logic: crc32, image_validate, can_update,
# dfu_config)
# -----------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt)
    add_subdirectory(tests)
endif()

# -----------------------------------------------------------------------------
# Bootloader firmware — custom target invoking the fork's Make build.
# Requires on PATH: GNU make, a POSIX sh, arm-none-eabi-gcc (see
# docs/build.md: the xPack gcc 14.2.1 toolchain). On Windows additionally pass
#   -DBOOTLOADER_SHELL=<absolute path to sh.exe, path without spaces>
# and build from a space-free checkout path (e.g. a `subst` drive).
# -----------------------------------------------------------------------------
# GNU make cannot handle paths containing spaces. If this checkout lives at
# a spaced path, map a space-free view (Windows: `subst B: <repo>`) and pass
# -DBOOTLOADER_SOURCE_DIR=B:/vendor/arduino-renesas-bootloader
set(BOOTLOADER_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendor/arduino-renesas-bootloader
    CACHE PATH "Space-free path to the vendored bootloader fork")
if(BOOTLOADER_SOURCE_DIR MATCHES " ")
    message(WARNING
        "BOOTLOADER_SOURCE_DIR contains spaces; the `bootloader` target will "
        "fail. Map a space-free view (e.g. `subst B: <repo>`) and set "
        "-DBOOTLOADER_SOURCE_DIR accordingly (docs/build.md).")
endif()

# gcc 7.x only: -DBOOTLOADER_CFLAGS_SKIP=-Wcast-function-type (gcc 7 does
# not know the flag tinyusb adds for gcc >= 8). Leave empty for gcc 14.
set(BOOTLOADER_CFLAGS_SKIP "" CACHE STRING "CFLAGS_SKIP passed to the bootloader make (gcc 7 only)")

set(BOOTLOADER_MAKE_ARGS
    -f Makefile.minima
    BOARD=uno_r4
    TOP=../tinyusb
    CURRENT_PATH=.
    CMDEXE=
)
if(BOOTLOADER_CFLAGS_SKIP)
    list(APPEND BOOTLOADER_MAKE_ARGS "CFLAGS_SKIP=${BOOTLOADER_CFLAGS_SKIP}")
endif()
if(DEFINED BOOTLOADER_SHELL)
    list(APPEND BOOTLOADER_MAKE_ARGS "BOOTLOADER_SHELL=${BOOTLOADER_SHELL}")
endif()

add_custom_target(bootloader
    COMMAND make ${BOOTLOADER_MAKE_ARGS}
    WORKING_DIRECTORY ${BOOTLOADER_SOURCE_DIR}
    COMMENT "Building UNO R4 Minima DFU+CAN bootloader (vendored Make build, 16 KB size gate included)"
    VERBATIM
    USES_TERMINAL
)
