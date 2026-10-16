# The brain's reset line to the node (GPIO3, inverted: HIGH = node in reset),
# shared by brain/ and examples/slcan_bridge.
idf_component_register(SRCS "node_reset.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_driver_gpio freertos)
