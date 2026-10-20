"""Select optional app helpers for each PlatformIO library builder.

The stock-core Arduino helper opts in with custom_arduino_node_app = yes.
The separate can_updater_client/recovery.c pair opts in with
custom_can_updater_client = yes: it requires the node bootloader's custom RTC
bytes in sdkconfig, which stock Arduino and brain builds do not reserve.

Owns: This builder's manifest srcFilter, never the shared source tree.
Invariants: pioarduino resets the builder cache and runs this script more than
    once per build. Extend only the current builder's manifest: library.json's
    srcFilter takes precedence over env SRC_FILTER.
See also: library.json, docs/guides/platformio-arduino.md.
"""
Import("env", "pio_lib_builder")

if env.GetProjectOption("custom_can_updater_client", "no").strip().lower() in ("yes", "true", "1"):
    build = pio_lib_builder._manifest["build"]
    build["srcFilter"] = list(build["srcFilter"]) + [
        "+<components/can_updater_client/can_updater_client.c>",
        "+<components/can_updater_client/recovery.c>",
    ]

if env.GetProjectOption("custom_arduino_node_app", "no").strip().lower() in ("yes", "true", "1"):
    build = pio_lib_builder._manifest["build"]
    build["srcFilter"] = list(build["srcFilter"]) + [
        "+<components/arduino_node_app/arduino_node_app.cpp>",
    ]
