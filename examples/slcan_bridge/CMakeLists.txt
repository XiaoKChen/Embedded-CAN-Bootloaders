# =============================================================================
# CMakeLists.txt — SLCAN USB-CAN bridge for the brain board
# =============================================================================
# Turns the brain (ESP32-S3-WROOM-1U-N16R8, TWAI TX GPIO6 / RX GPIO7) into an
# SLCAN adapter on its native USB port, so tools/can-flash reaches the node's
# updater with `--interface slcan` (docs/guides/slcan-bridge.md). From the repo root:
#   powershell -ExecutionPolicy Bypass -File build.ps1 examples/slcan_bridge
#   ./build.sh examples/slcan_bridge
# Flash it onto the brain over USB (it is a plain app, not an OTA image):
#   build.ps1 examples/slcan_bridge -p COM6 flash
# =============================================================================

cmake_minimum_required(VERSION 3.16)

# The TWAI wrapper, the node's reset line and the SLCAN codec are used
# straight from the repo.
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../../components/can_link"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/node_reset"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/slcan_codec")
set(COMPONENTS main)

# Windows: as for examples/bench_app, object paths in a long checkout pass
# CMake's conservative 250-character default while still fitting MAX_PATH.
if(CMAKE_HOST_WIN32)
    set(CMAKE_OBJECT_PATH_MAX 259)
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(slcan_bridge)
