idf_component_register(SRCS "brain_ota.c" "ota_update.c" "eth_w5500.c" "http_ota_server.c"
                       PRIV_REQUIRES app_update esp_app_format esp_partition esp_timer
                                     esp_driver_gpio esp_driver_spi esp_eth esp_netif
                                     esp_event esp_http_server)
