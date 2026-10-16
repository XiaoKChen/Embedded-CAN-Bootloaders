# app_control.c is the pure codec shared with node apps (src/boot/); boot_core
# does not compile it, so the host side builds it here.
idf_component_register(SRCS "can_flash_host.c" "${CMAKE_CURRENT_LIST_DIR}/../../src/boot/app_control.c"
                       INCLUDE_DIRS "include"
                       REQUIRES boot_core)
