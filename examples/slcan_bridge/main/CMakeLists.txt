idf_component_register(SRCS "slcan_bridge.c"
                       PRIV_REQUIRES can_link node_reset slcan_codec
                                     esp_driver_usb_serial_jtag)
