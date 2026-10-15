# TWAI (classic CAN) link wrapper shared by the updater (node_updater/) and
# the SLCAN bridge (examples/slcan_bridge). No IDF type leaks through its
# header.
idf_component_register(SRCS "can_link.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES driver)
