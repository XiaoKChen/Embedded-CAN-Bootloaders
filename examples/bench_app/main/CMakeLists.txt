idf_component_register(SRCS "bench_app.c"
                       PRIV_REQUIRES can_updater_client app_update esp_app_format
                                     esp_vfs_console)  # stdin on the console port
