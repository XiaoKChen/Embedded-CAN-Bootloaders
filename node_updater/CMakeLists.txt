# =============================================================================
# CMakeLists.txt — ESP32-S3 CAN (TWAI) updater (ESP-IDF project, the node's)
# =============================================================================
# Builds the `factory` app (main/), the recovery 2nd-stage bootloader configured
# by sdkconfig.defaults, and the partition table (partitions.csv).
#   One command, tools included:  ./build.sh node_updater  (see README)
#   With ESP-IDF v5.3 exported:   idf.py set-target esp32s3 build
# Host tests for the shared pure-logic core are a separate CMake project:
#   cmake -S tests -B build-host  (see tests/CMakeLists.txt)
# =============================================================================

cmake_minimum_required(VERSION 3.16)

# The shared components (boot_core, can_link, ...) are in the repo's components/.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../components")

# Build only what the updater links (plus the app-side client component, so
# it is compile-checked) instead of every component ESP-IDF ships.
set(COMPONENTS main can_updater_client)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Windows: in a long checkout, the bootloader sub-project's object paths pass
# CMake's 250-character default while still fitting MAX_PATH; raise it there.
if(CMAKE_HOST_WIN32)
    idf_build_set_property(EXTRA_CMAKE_ARGS "-DCMAKE_OBJECT_PATH_MAX=259" APPEND)
endif()

project(can_updater)
