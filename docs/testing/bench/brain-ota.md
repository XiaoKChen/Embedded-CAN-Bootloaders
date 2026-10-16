# Bench: brain OTA over Ethernet

Tests the **brain** updating itself over its W5500 Ethernet port: an image
POSTed to `/ota` goes into the inactive OTA slot, the brain reboots into it,
and a self-test marks it valid or rolls it back. The node, CAN and can-flash
play no part here. Design: [system-overview.md, the brain](../../concepts/system-overview.md#the-brain);
API: [`POST /ota`](../../reference/brain-http-api.md#post-ota).

History: issue #5. Common setup: [README.md](README.md). The brain's port is
**COM63** below.

## Build

```sh
powershell -ExecutionPolicy Bypass -File build.ps1 brain
./build.sh brain
```

(`build.ps1` / `./build.sh` with no argument build it too, after the node
updater.) The OTA image is `brain/build/brain_ota.bin` (about 440 KB).

**The "bad" image for test (2)** skips `esp_ota_mark_app_valid_cancel_rollback()`
(Kconfig `BRAIN_OTA_SKIP_MARK_VALID`). It is built in its own short build
dir, as for the bench app:

```sh
powershell -ExecutionPolicy Bypass -File build.ps1 brain -B skip -D SDKCONFIG=skip/sdkconfig -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.skip_mark_valid" app
./build.sh brain -B skip -D SDKCONFIG=skip/sdkconfig -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.skip_mark_valid" app
```

Result: `brain/skip/brain_ota.bin`.

## First flash over USB

Follow [first-flash.md, the brain](../../getting-started/first-flash.md#the-brain-brain_ota)
(`build.ps1 brain -p COM63 flash monitor`) and note the IP from the
`IP acquired` line. Nothing else here works until `IP acquired` shows.
Flashing `brain_ota` replaces the SLCAN bridge; re-flash the bridge over USB
to return to CAN bench work.

## Status and upload

```sh
curl.exe http://<ip>/
curl.exe --data-binary "@brain/build/brain_ota.bin" http://<ip>/ota
```

The status page and the upload's replies and error codes:
[brain-http-api.md](../../reference/brain-http-api.md#get-).

## Tests

Keep `monitor` open on COM63 through the tests. Byte counts below are for a
449 568-byte image.

**Opening COM63 can reset the brain.** Observed on hardware: a tool that
(re)opens the port with pyserial's default DTR/RTS (a plain script, or
`monitor` without `--no-reset`) resets the S3. During test (2) that reset
alone triggers the rollback before you mean it to. Either keep one `monitor`
session open the whole time, or watch the state with `curl.exe http://<ip>/`
instead of the console.

Results on the real brain (2026-10-14, 172.16.5.71): (1) 449 568 B in about
3.3 s, `PENDING_VERIFY` → VALID; (2) bad image `PENDING_VERIFY`, a second
upload refused with 409, reset → rolled back, VALID; (3) cable pulled mid
upload → no reboot, still the old slot, VALID; (4) power cut mid upload →
old slot, VALID.

### (1) Good image: reboot, self-test, VALID

```sh
curl.exe --data-binary "@brain/build/brain_ota.bin" http://<ip>/ota
```

```
I (...) http_ota: POST /ota: 449568 bytes announced
I (...) ota_update: OTA begin: 449568 bytes into ota_1 (offset 0x320000, 3145728 bytes)
I (...) ota_update: OTA progress:  10 % (... / 449568 bytes)
...
I (...) ota_update: OTA progress: 100 % (449568 / 449568 bytes)
I (...) ota_update: OTA result: 449568 bytes written and validated; ota_1 boots next
I (...) http_ota: restarting in 1000 ms
...
I (...) brain_ota: brain_ota version ... running from ota_1 (0x320000), OTA state PENDING_VERIFY
I (...) brain_ota: self-test: new image, waiting up to 30 s for an IP
I (...) eth_w5500: IP acquired: ...
I (...) brain_ota: self-test passed; image marked VALID (rollback cancelled)
```

`curl.exe http://<ip>/` now shows `running: ota_1` and `ota state: VALID`.
Reset the brain (Ctrl+T Ctrl+R): it boots `ota_1` again, `OTA state VALID`.
Each further upload goes to the other slot.

### (2) Bad (SKIP_MARK_VALID) image: rollback on the next reset

```sh
curl.exe --data-binary "@brain/skip/brain_ota.bin" http://<ip>/ota
```

It is written and booted as in (1), but stays unconfirmed:

```
I (...) brain_ota: brain_ota version ... running from ota_0 (0x20000), OTA state PENDING_VERIFY
I (...) brain_ota: self-test: new image, waiting up to 30 s for an IP
I (...) eth_w5500: IP acquired: ...
W (...) brain_ota: self-test passed, but this SKIP_MARK_VALID build leaves the image unconfirmed; the next reset rolls back to the previous slot
```

`GET /` shows `ota state: PENDING_VERIFY`, and an upload now gets `409`.
Reset the brain (Ctrl+T Ctrl+R, the reset button, or a power cycle). The
bootloader marks the unconfirmed slot `ABORTED` and boots the previous one:

```
I (...) brain_ota: brain_ota version ... running from ota_1 (0x320000), OTA state VALID
```

A fresh image that gets no IP within 30 s (booted with the cable out, say)
rolls back by itself, without a reset:

```
E (...) brain_ota: self-test failed (no network within 30 s); rolling back and rebooting
```

### (3) Ethernet cable pulled mid-upload: abort, old image still boots

Slow the upload down so there is time to act. At 8 KB/s the image takes
about 55 s; `--limit-rate 20k` (about 22 s) also works if you are quick.

```sh
curl.exe --limit-rate 8k --data-binary "@brain/build/brain_ota.bin" http://<ip>/ota
```

Pull the brain's Ethernet cable at around 50 %:

```
I (...) ota_update: OTA progress:  50 % (... / 449568 bytes)
W (...) eth_w5500: link down
E (...) http_ota: no data for 10 s after ... of 449568 bytes (cable pulled?)
W (...) ota_update: OTA result: aborted after ... of 449568 bytes; boot partition unchanged
```

curl does not notice at once (TCP keeps retrying); stop it with Ctrl+C.
Plug the cable back in (`link up`, then `IP acquired` again). `GET /` shows
the same running slot, `VALID`; a reset boots that same slot; a new upload
succeeds.

### (4) Power cut mid-upload: old image still boots

Start the slowed upload as in (3) and cut the brain's power (unplug its USB)
at around 50 %. curl fails. Restore power: `otadata` was never touched, so
the brain boots the slot it ran before, `OTA state VALID`, and gets its IP
again. The half-written slot is overwritten by the next upload.

## GPIO3 and the node during brain reboots

Every OTA reboot, rollback and reset of the brain can hold the node in reset
briefly, and the node restarts; a node connected over USB re-enumerates.
That is accepted for this bench:
[wiring.md, GPIO3](../../getting-started/wiring.md#brain-gpio3--node-en-reset-line).
