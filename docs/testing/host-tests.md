# Host tests

Unity/CTest suites for the shared pure logic, built for the PC. No ESP-IDF or
hardware needed.

```sh
cmake -S tests -B build-host && cmake --build build-host && ctest --test-dir build-host -C Debug --output-on-failure
```

or step by step:

```sh
cmake -S tests -B build-host
cmake --build build-host --config Debug
ctest --test-dir build-host -C Debug --output-on-failure
```

`-C Debug` is needed with multi-config generators (the Visual Studio default
on Windows) and ignored by single-config ones. CI
(`.github/workflows/esp32-build.yml`, [build.md](../getting-started/build.md#ci-and-releases))
already runs all CTest suites.

## What they cover

- The shared pure-logic core in `src/boot/`: protocol engine, CRC32,
  config-block parser, app-control codec (`test_app_control`, with golden
  vectors shared with `can_flash.py --selftest`), package parser
  (`test_update_pkg`), boot counter, and the RA4M1 image validator kept as
  prior art.
- The SLCAN codec in `components/slcan_codec/`.
- **Host/node integration:** `test_can_flash_host` compiles
  `components/can_flash_host/` against the real node engine with a simulated
  CAN bus, RAM slot and fake clock (loss/retry/wrap, CRC and discovery tests).
  The suite includes >256 KB/wrap recovery, lost/duplicate DATA, lost
  intermediate and final ACKs, final-frame loss, node reset/restart,
  CRC/image rejection, size/timeout errors, three-node discovery and every
  name length 0..32. A 5099-byte deterministic image was cross-checked against
  the actual Python `CanFlasher`: all **857 frames / 6856 payload bytes**
  (HELLO through GO, including padding and sequence bytes) matched, CRC32
  `0xBBCFA65A`.
- **Recovery counters:** threshold, sticky trip, clear, one-use clean marks,
  RTC garbage, every single weak programmed bit, erased/garbage logs, and
  full-log wrap ([recovery.md](../concepts/recovery.md)).

Builds and host tests do not prove real EN timing, RTC retention, brownout
flash behaviour, HTTP task stack headroom, watchdogs, NVS interruption or real
GPIO/CAN timing: run the [bench tests](bench/README.md) before deployment.

Other host-side checks: `can_flash.py --selftest`
([tools/can-flash/README.md](../../tools/can-flash/README.md#selftest)) and
the package vector / `ecb_pkg.py info`
([tools/pkg/README.md](../../tools/pkg/README.md#parser-interoperability-vector)).
