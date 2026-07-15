# =============================================================================
# tests/CMakeLists.txt — Host-side Unity tests for the bootloader's pure logic
# =============================================================================
# Standalone:  cmake -S tests -B build-host && cmake --build build-host
#              && ctest --test-dir build-host --output-on-failure
# Also usable via add_subdirectory(tests) from the root CMakeLists.
# =============================================================================

cmake_minimum_required(VERSION 3.20)

project(bootloader_host_tests LANGUAGES C)

set(CMAKE_C_STANDARD          11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS       OFF)

enable_testing()

if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra)
endif()

set(BOOT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../src/boot)

# Vendored Unity (v2.6.1: unity.c + unity.h + unity_internals.h).
add_library(unity STATIC ${CMAKE_CURRENT_SOURCE_DIR}/unity/unity.c)
target_include_directories(unity PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/unity)

# Pure-logic bootloader core under test — zero hardware includes by contract.
add_library(boot_core STATIC
    ${BOOT_DIR}/crc32.c
    ${BOOT_DIR}/image_validate.c
    ${BOOT_DIR}/can_update.c
    ${BOOT_DIR}/dfu_config.c
)
target_include_directories(boot_core PUBLIC ${BOOT_DIR})

function(add_host_test name)
    add_executable(${name} ${CMAKE_CURRENT_SOURCE_DIR}/${name}.c)
    target_link_libraries(${name} PRIVATE boot_core unity)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

add_host_test(test_crc32)
add_host_test(test_image_validate)
add_host_test(test_can_update)
add_host_test(test_dfu_config)
