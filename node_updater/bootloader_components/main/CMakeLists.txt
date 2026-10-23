# Purpose: Build early board holds, I2C safe writes and the recovery bootloader override.
# Summary: Compile pure profile/CRC/I2C logic while retaining IDF's linker and loader.
set(BOOT_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../src/boot")
idf_component_register(SRCS "boot.c" "${BOOT_DIR}/boot_counter.c"
                            "${BOOT_DIR}/board_profile.c" "${BOOT_DIR}/crc32.c"
                            "${BOOT_DIR}/i2c_bitbang.c"
                       INCLUDE_DIRS "${BOOT_DIR}"
                       REQUIRES bootloader bootloader_support spi_flash)
idf_build_get_property(target IDF_TARGET)
target_linker_script(${COMPONENT_LIB} INTERFACE
    "${IDF_PATH}/components/bootloader/subproject/main/ld/${target}/bootloader.ld"
    "${IDF_PATH}/components/bootloader/subproject/main/ld/${target}/bootloader.rom.ld")
