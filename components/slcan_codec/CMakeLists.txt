# Pure-logic SLCAN (Lawicel) codec: no ESP-IDF includes, host-tested from
# tests/test_slcan_codec.c. Used by examples/slcan_bridge.
idf_component_register(SRCS "slcan_codec.c"
                       INCLUDE_DIRS "include")
