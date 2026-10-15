# =============================================================================
# CMakeLists.txt — brain self-update over Ethernet (W5500) and HTTP
# =============================================================================
# Bench firmware for the brain (ESP32-S3-WROOM-1U-N16R8): an image POSTed to
# http://<ip>/ota goes into the inactive OTA slot, the board reboots, and a
# self-test marks it valid or rolls it back (docs/bench-test.md, section 8).
# From the repo root:
#   powershell -ExecutionPolicy Bypass -File build.ps1 examples/brain_ota
#   ./build.sh examples/brain_ota
# First flash over USB (its own 16 MB partition table, not the node's):
#   build.ps1 examples/brain_ota -p COM63 flash
# =============================================================================

cmake_minimum_required(VERSION 3.16)

set(COMPONENTS main)

# Windows: as for examples/slcan_bridge, object paths in a long checkout pass
# CMake's conservative 250-character default while still fitting MAX_PATH.
if(CMAKE_HOST_WIN32)
    set(CMAKE_OBJECT_PATH_MAX 259)
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(brain_ota)
