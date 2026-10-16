# Bench: brain flashes the node over CAN

Tests the brain's C CAN host: the PC supplies the image over Ethernet; it is
**not on the CAN bus**. The brain runs `brain_ota`, not the SLCAN bridge. The
node must already be parked in its updater: use the bench app's `u` command,
`POST /node/recover` ([node-recovery.md](node-recovery.md)), or the USB
`otadata` erase ([recovery.md](../../concepts/recovery.md)). **GPIO3 stays LOW
throughout staging, scanning and flashing.** Brain reboot caveats
([GPIO3](../../getting-started/wiring.md#brain-gpio3--node-en-reset-line))
still apply. API: [brain-http-api.md](../../reference/brain-http-api.md).

History: Phase 2, issue #8. **Bench-only, trusted LAN:** these HTTP routes
have no authentication or TLS. Common setup: [README.md](README.md).

## Prepare and discover

Build both firmwares and the node app, then flash the **brain first** over
USB (replace COM63 with its actual port):

```sh
powershell -ExecutionPolicy Bypass -File build.ps1
powershell -ExecutionPolicy Bypass -File build.ps1 examples/bench_app
powershell -ExecutionPolicy Bypass -File build.ps1 brain -p COM63 flash
```

If BOOT was held for download mode and it stays there after flashing,
power-cycle without BOOT. Let the brain acquire a DHCP address
([first-flash.md](../../getting-started/first-flash.md#the-brain-brain_ota)).
Put the node into its updater; if it has never been installed, follow
[the node's USB flash](../../getting-started/first-flash.md#the-node-the-updater),
using the node's actual port. Connect CANH/CANL/GND and the two 120-ohm
terminators ([wiring.md](../../getting-started/wiring.md#brain--node)).

```sh
curl.exe http://<ip>/node/scan
curl.exe "http://<ip>/node/scan?from=0x700&to=0x73F"
```

With one unprovisioned node, expect (IDs are decimal in JSON):

```json
{"nodes":[{"cmd_id":1950,"reply_id":1951,"name":"","protocol":1}],"truncated":false}
```

An empty/default scan takes about **11.4 s**. Scan rules, `truncated`, 400 and
409: [`GET /node/scan`](../../reference/brain-http-api.md#get-nodescan).

## Stage, flash, verify and boot

```sh
curl.exe -i --data-binary "@examples/bench_app/build/bench_app.bin" "http://<ip>/node/flash?cmd=0x79E&reply=0x79F"
curl.exe http://<ip>/node/status
```

Expect **202** once the body is verified in staging, then poll status:

```json
{"state":"flashing","percent":42,"bytes":81920,"total":191264,"status":"CAN transfer; awaiting VERIFY and GO"}
```

On success expect `state:success`, `percent:100`, equal `bytes`/`total`, and
`status` beginning `GO: success`. On the node's console expect the normal
bench-app boot and `self-test passed; image marked valid`, as in
[node-updater.md (b)](node-updater.md#b-flash-the-bench-app-over-can-go-app-confirms-itself).
The brain does not reboot and its own running slot is unchanged. A running
bench app no longer answers INFO; use `u` again before the next transfer.

## Failure and isolation checks (run only on an available bench)

- **Early replies on Windows:** the brain answers 409/411/413 as soon as it
  reads the headers, then closes. With a large body still being sent, Windows
  curl may report `000` / "Connection was reset" and drop the reply; `-v`
  shows the status line, or send a small body to see it cleanly.
- **Conflicts, both directions:** start a slow node upload with
  `curl.exe --limit-rate 8k --data-binary "@examples/bench_app/build/bench_app.bin" "http://<ip>/node/flash"`.
  In another terminal POST again to `/node/flash`, then POST the brain image
  to `/ota`; each must return **409**, during staging and during CAN flashing.
  `/node/status` must still respond. Repeat with a slow `/ota` upload and a
  concurrent `/node/flash`: again 409. Abort the slow self-upload before
  completion if you do not intend to reboot the brain.
- **Dropped upload:** cancel curl or pull Ethernet during staging. After the
  10 s receive timeout (or immediate peer close), status is `failed`; no CAN
  session starts, neither boot selection changes, and a fresh upload works.
  Missing length returns 411; a body larger than the **2 MB staging partition**
  returns 413 before erase. A staged image bigger than the node's announced
  **1.75 MB slot** gets `SIZE rejected: size out of bounds 0x04`, before node
  ERASE. A complete upload whose 202 response cannot be sent is not flashed.
- **No node / CAN disconnected:** scan returns no hits on a healthy bus, or a
  transport error if TX cannot drain. Flash ends `failed` with a HELLO timeout
  or transport failure, never success. Disconnect CAN mid-DATA, then reconnect
  and retry the entire HTTP upload after failure. Inspect the brain's final
  `CAN health` log for bus state, TX/RX error counters and dropped frames.
- **Node reset/power loss mid-transfer:** the node returns to its updater;
  this attempt fails with a clear DATA/transport status. A fresh POST starts
  a new HELLO session and succeeds. No automatic reset is driven by the brain.
- **Rejected image:** a body whose first byte is not the ESP image magic `0xE9`
  is refused by the node's first `esp_ota_write`, so it ends `failed` with
  `DATA window 0 not accepted after 4 attempts`. To reach the VERIFY check,
  upload garbage that starts with `0xE9` (e.g. 4 KB random with the first byte
  replaced): it ends `failed` with `VERIFY rejected: image rejected 0x06`.
  Either way the node stays in its updater. CRC mismatch is independently
  covered by host fault tests.
- **Brain power loss during staging:** after reboot, status is `idle`. Length
  and validity are RAM-only, so partial/stale `node_fw` contents are never
  resumed. During CAN flashing, power loss leaves the node in its updater
  unless GO already selected the verified app. Re-upload to start fresh.
- **Own OTA regression:** repeat [brain-ota.md](brain-ota.md#tests)'s normal
  image, skip-mark-valid rollback, dropped upload and power-cut checks. The
  self-test and A/B OTA implementation are unchanged; a single async HTTP
  worker now keeps conflict and status replies responsive while either upload
  is active.

## Host compatibility

The C host's timing, retries and intentional differences from the Python
host: [can-protocol.md](../../reference/can-protocol.md#host-implementations-and-known-limits).
Host verification without hardware: [host-tests.md](../host-tests.md).
