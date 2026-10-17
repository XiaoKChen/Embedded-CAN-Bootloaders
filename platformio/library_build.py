# PlatformIO extraScript of library.json (docs/guides/platformio-arduino.md).
# can_updater_client.c and recovery.c are node-only: recovery.c #errors unless
# the app's sdkconfig reserves the node bootloader's custom RTC bytes, which a
# brain build does not. A node environment opts in with
# `custom_can_updater_client = yes`.
import os

Import("env", "pio_lib_builder")

if env.GetProjectOption("custom_can_updater_client", "no").strip().lower() in ("yes", "true", "1"):
    client_dir = os.path.join("components", "can_updater_client")
    env.BuildSources(
        os.path.join(pio_lib_builder.build_dir, client_dir),
        os.path.join(pio_lib_builder.path, client_dir),
        ["+<can_updater_client.c>", "+<recovery.c>"],
    )
