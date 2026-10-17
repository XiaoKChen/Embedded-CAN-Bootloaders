# PlatformIO extraScript of library.json (docs/guides/platformio-arduino.md).
# can_updater_client.c and recovery.c are node-only: recovery.c #errors unless
# the app's sdkconfig reserves the node bootloader's custom RTC bytes, which a
# brain build does not. A node environment opts in with
# `custom_can_updater_client = yes`.
#
# This script runs once per library builder, and pioarduino creates builders
# more than once per build (espidf.py resets the builder cache). So it only
# extends this builder's srcFilter, which the builder reads when it is built;
# library.json's srcFilter takes precedence over env SRC_FILTER, so it is
# edited in the builder's manifest.
Import("env", "pio_lib_builder")

if env.GetProjectOption("custom_can_updater_client", "no").strip().lower() in ("yes", "true", "1"):
    build = pio_lib_builder._manifest["build"]
    build["srcFilter"] = list(build["srcFilter"]) + [
        "+<components/can_updater_client/can_updater_client.c>",
        "+<components/can_updater_client/recovery.c>",
    ]
