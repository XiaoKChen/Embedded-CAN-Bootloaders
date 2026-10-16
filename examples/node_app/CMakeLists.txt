# =============================================================================
# CMakeLists.txt — reference node application (Phase 6, issue #11)
# =============================================================================
# The starting point for real node firmware: an OTA app for the node's 4 MB
# layout that tracks Phase 3 health, confirms itself after a self-test, and
# answers APP_INFO / ENTER_UPDATER on its own CAN update IDs so the brain can
# park it in the CAN updater without the GPIO3 reset line (docs/bench-test.md,
# section 12). From the repo root:
#   powershell -ExecutionPolicy Bypass -File build.ps1 examples/node_app
#   ./build.sh examples/node_app
# OTA image: examples/node_app/build/node_app.bin, delivered over CAN (brain
# package or tools/can-flash). Never `idf.py flash` it onto an updater board:
# it would land in `factory` and replace the updater.
# =============================================================================

cmake_minimum_required(VERSION 3.16)

# Shared components straight from the repo: the updater client (health +
# updater entry), the TWAI wrapper, and the pure-logic core (cancfg parsing,
# CRC32 for the APP_INFO check byte).
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../../components/can_updater_client"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/can_link"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/boot_core")
set(COMPONENTS main)

# Windows: as for examples/bench_app, object paths in a long checkout pass
# CMake's conservative 250-character default while still fitting MAX_PATH.
if(CMAKE_HOST_WIN32)
    set(CMAKE_OBJECT_PATH_MAX 259)
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(node_app)
