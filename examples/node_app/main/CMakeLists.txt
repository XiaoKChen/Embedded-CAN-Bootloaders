# app_control.c is the pure APP_INFO / ENTER_UPDATER codec shared with the
# brain's CAN host (src/boot/, host-tested); boot_core does not compile it.
idf_component_register(SRCS "node_app.c" "can_service.c" "../../../src/boot/app_control.c"
                       PRIV_REQUIRES can_updater_client can_link boot_core
                                     app_update esp_app_format esp_partition)
