# Bench: node recovery

Tests software-only recovery: the RTC crash-loop counter and the brain-forced
EN-pulse counter over the existing inverted GPIO3-to-EN wire. How it works,
thresholds and stable-clear rules: [recovery.md](../../concepts/recovery.md).
The brain runs `brain_ota`, not the SLCAN bridge. HTTP recovery is
unauthenticated: use a trusted bench LAN only.

History: Phase 3, issue #9. Common setup: [README.md](README.md).

## Install once over USB

**Existing nodes need bootloader + partition table + updater flashed over
USB once. The bootloader cannot be updated over CAN.** Follow
[first-flash.md](../../getting-started/first-flash.md#the-node-the-updater),
with the brain already running to release the node's EN:

```sh
powershell -ExecutionPolicy Bypass -File build.ps1 node_updater
powershell -ExecutionPolicy Bypass -File build.ps1 node_updater -p COM7 flash
```

This writes bootloader `0x0`, table `0x8000`, initial otadata `0xe000`, and
updater `0x20000`. It resets OTA selection but preserves app slots and
`cancfg`; do not add `erase-flash` if you need those. **`bootcnt`, data subtype
`0x42`, occupies the previously unused `0x12000..0x12fff`.** All existing
partition offsets and sizes are unchanged. Leave the updater running until
`node_health: 10 s stable uptime: recovery counters cleared` before testing.

Regenerate stale `sdkconfig` files and keep every app on the RTC contract:
[configuration.md](../../reference/configuration.md#committed-sdkconfigdefaults),
[writing-a-node-app.md](../../guides/writing-a-node-app.md#recovery-contract).
The updater and bench variants already call `can_updater_client_start_health()`.

## Confirmed-app crash loop

Build a separate short `crash` directory, alongside normal `build` and `skip`:

```sh
powershell -ExecutionPolicy Bypass -File build.ps1 examples/bench_app -B crash -D SDKCONFIG=crash/sdkconfig -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.crash_loop" app
./build.sh examples/bench_app -B crash -D SDKCONFIG=crash/sdkconfig -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.crash_loop" app
```

The resulting `examples/bench_app/crash/bench_app.bin` marks itself **VALID**,
waits about 2 seconds and `abort()`s on every boot. Unlike `skip`, this tests
recovery of a confirmed image, not pending-image rollback. The `skip` build
([node-updater.md](node-updater.md#2-build)) is unchanged.

1. Park the node (`u` or recovery below), then wait for its 10 s stable-clear
   message so the test starts with zero counters.
2. Flash through the brain's [`/node/flash`](brain-can-flash.md) API:

   ```sh
   curl.exe -i --data-binary "@examples/bench_app/crash/bench_app.bin" "http://<ip>/node/flash?cmd=0x79E&reply=0x79F"
   curl.exe http://<ip>/node/status
   ```

3. Flash `success` means VERIFY/GO succeeded, not that the app stays healthy.
   Observe three abort resets. On the boot after abort 3, expect
   `recovery: latched recovery -> factory (RTC 3, flash 1)`, then
   `can_updater: parked`. Here `flash 1` is a boolean latch, not a count.
4. Reset/EN-pulse factory before 10 s: it must remain in factory. Flash a
   verified image immediately, without waiting for stable clear: GO clears
   both counters before selecting/restarting the image. A small image that
   reaches GO within 10 s must boot too. It must mark valid and keep running.

Bootloader logs may only appear on UART0; USB re-enumeration can hide early
boots. Use UART capture/logic analyzer for early timing and do not confuse
opening a resetting serial monitor with a passive observation.

## Brain-forced recovery

Start with a normal VALID bench app running for more than 10 s. No app CAN
handler is required. Use one of these alternative requests:

```sh
curl.exe -i -X POST http://<ip>/node/recover
curl.exe -i -X POST "http://<ip>/node/recover?cmd=0x79E&reply=0x79F"
curl.exe http://<ip>/node/status
```

POST returns **202**. Poll `GET /node/status` for `recovering`, then
`success` or `failed`. For provisioned nodes supply both actual IDs. The
brain sends **four pulses**, each **GPIO3 HIGH 100 ms** (node held reset),
then **LOW 400 ms** (node runs), finally leaving LOW. From a healthy erased
log, release 3 must select factory, and release 4 must still select factory.

Verify EN-release-to-record latency is **well below 400 ms on hardware**; a
firmware build alone cannot measure it. `success` means the brain reached the
updater. Immediately flash via [`/node/flash`](brain-can-flash.md), even
before 10 s: GO clears both counters first. Verify the normal app boots;
repeat with a small image whose GO occurs inside 10 s. Disconnect CAN and
repeat: the node may park physically, but the brain must report failure
without a valid updater reply.

## Normal resets, stable clear, and fault injection

- Let the normal app run 10 s, power-cycle once, and confirm it boots normally.
  Let it run 10 s again before another cycle. This must never trip. Three
  rapid ordinary power cycles intentionally look like a recovery request.
- Repeat flash/`u` re-entry. Deliberate `esp_restart()` must not increment the
  RTC crash count; its clean mark is consumed on the next software reset only.
- Reset at 9 s: evidence remains. Confirm the clear log after 10 s of
  uninterrupted uptime.
- After a forced recovery and stable clear, the updater keeps running, but a
  later reset may boot the old selected app; `u` instead erases otadata and
  remains parked.
- Interrupt each flash mutation (bit-log append, untripped wrap, stable/GO
  clear, RTC update) with EN or power loss and compare against the
  [outcome table](../../concepts/recovery.md#fail-safe-and-power-loss).

Host tests cover the counter logic ([host-tests.md](../host-tests.md)).
Builds do not prove real EN timing, RTC retention, or brownout flash
behavior: run the hardware checks above before deployment.
