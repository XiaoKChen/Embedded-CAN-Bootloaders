# Purpose: Build the board-profiled CAN updater and USB CDC recovery service.
# Summary: Reuse boot_core's CRC/config codec; compile the board codec and I2C bit-bang here.
set(BOOT_DIR "${CMAKE_CURRENT_LIST_DIR}/../../src/boot")
idf_component_register(SRCS "main.c" "usb_cdc.c" "${BOOT_DIR}/board_profile.c"
                            "${BOOT_DIR}/i2c_bitbang.c"
                       INCLUDE_DIRS "."
                       REQUIRES boot_core can_link app_update esp_partition can_updater_client
                                esp_driver_gpio esp_hw_support esp_tinyusb)
