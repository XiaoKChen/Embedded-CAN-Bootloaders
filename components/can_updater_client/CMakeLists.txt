set(BOOT_DIR "${CMAKE_CURRENT_LIST_DIR}/../../src/boot")
idf_component_register(SRCS "can_updater_client.c" "recovery.c" "${BOOT_DIR}/boot_counter.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "${BOOT_DIR}"
                       PRIV_REQUIRES app_update esp_partition bootloader_support)
