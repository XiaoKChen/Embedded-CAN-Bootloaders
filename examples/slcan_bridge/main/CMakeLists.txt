idf_component_register(SRCS "slcan_bridge.c"
                       PRIV_REQUIRES can_link slcan_codec esp_driver_gpio
                                     esp_driver_usb_serial_jtag)
