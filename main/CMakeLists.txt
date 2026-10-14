idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES boot_core can_link app_update esp_partition)
