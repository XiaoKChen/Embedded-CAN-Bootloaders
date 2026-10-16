idf_component_register(SRCS "brain_ota.c" "ota_update.c" "eth_w5500.c" "http_ota_server.c" "node_flash.c"
                            "package_update.c" "../../src/boot/update_pkg.c"
                       PRIV_REQUIRES app_update esp_app_format esp_partition esp_timer
                                     bootloader_support boot_core mbedtls nvs_flash
                                     esp_driver_gpio esp_driver_spi esp_eth esp_netif
                                     esp_event esp_http_server node_reset can_link can_flash_host)
