# ESP-IDF v5.3.6 bootloader_override seam; retain IDF's linker and image loader.
set(BOOT_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../src/boot")
idf_component_register(SRCS "boot.c" "${BOOT_DIR}/boot_counter.c"
                       INCLUDE_DIRS "${BOOT_DIR}"
                       REQUIRES bootloader bootloader_support spi_flash)
idf_build_get_property(target IDF_TARGET)
target_linker_script(${COMPONENT_LIB} INTERFACE
    "${IDF_PATH}/components/bootloader/subproject/main/ld/${target}/bootloader.ld"
    "${IDF_PATH}/components/bootloader/subproject/main/ld/${target}/bootloader.rom.ld")
