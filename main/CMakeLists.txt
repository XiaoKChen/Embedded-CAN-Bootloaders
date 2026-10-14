idf_component_register(SRCS "main.c" "can_link.c"
                       INCLUDE_DIRS "."
                       REQUIRES boot_core driver app_update esp_partition)
