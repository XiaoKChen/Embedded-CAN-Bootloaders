# The shared pure-logic core lives in src/boot/ (host-tested from tests/);
# this component only compiles it into the firmware.
set(boot_dir "${CMAKE_CURRENT_LIST_DIR}/../../src/boot")

idf_component_register(SRCS "${boot_dir}/can_update.c"
                            "${boot_dir}/crc32.c"
                            "${boot_dir}/dfu_config.c"
                       INCLUDE_DIRS "${boot_dir}")
