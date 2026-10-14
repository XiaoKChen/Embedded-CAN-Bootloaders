# =============================================================================
# CMakeLists.txt — bench-test app for the ESP32-S3 CAN updater
# =============================================================================
# A minimal OTA application delivered over CAN by tools/can-flash
# (docs/bench-test.md). From the repo root:
#   powershell -ExecutionPolicy Bypass -File build.ps1 examples/bench_app
#   ./build.sh examples/bench_app
# OTA image: examples/bench_app/build/bench_app.bin. Never `idf.py flash` it
# onto an updater board: it would land in `factory` and replace the updater.
# =============================================================================

cmake_minimum_required(VERSION 3.16)

# The client is used straight from the repo, not copied.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components/can_updater_client")
set(COMPONENTS main)

# Windows: this project sits two levels deeper than the updater, so in a long
# checkout its object paths pass CMake's conservative 250-character default
# while still fitting MAX_PATH (260 including the NUL). Build it with
# `idf.py app` (the scripts' default): its unused bootloader sub-project goes
# deeper still.
if(CMAKE_HOST_WIN32)
    set(CMAKE_OBJECT_PATH_MAX 259)
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(bench_app)
