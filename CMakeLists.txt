# =============================================================================
# CMakeLists.txt — ESP32-S3 CAN (TWAI) updater (ESP-IDF project, repo root)
# =============================================================================
# Builds the `factory` app (main/), the stock 2nd-stage bootloader configured
# by sdkconfig.defaults, and the partition table (partitions.csv).
#   One command, tools included:  ./build.sh   or   build.ps1  (see README)
#   With ESP-IDF v5.3 exported:   idf.py set-target esp32s3 build
# Host tests for the shared pure-logic core are a separate CMake project:
#   cmake -S tests -B build-host  (see tests/CMakeLists.txt)
# =============================================================================

cmake_minimum_required(VERSION 3.16)

# Build only what the updater links (plus the app-side client component, so
# it is compile-checked) instead of every component ESP-IDF ships.
set(COMPONENTS main can_updater_client)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(can_updater)
