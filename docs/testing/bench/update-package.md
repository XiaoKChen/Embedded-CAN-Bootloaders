# Bench: update package

Tests the brain-first update package and resumable node relay. **Run these
hardware steps only when the bench is available.** Packages are unsigned and
HTTP has no authentication: trusted bench LAN only. The brain runs
`brain_ota`, not the USB-CAN bridge. See
[package-format.md](../../reference/package-format.md) for the wire format,
limits, NVS record and power-loss table;
[brain-http-api.md](../../reference/brain-http-api.md#get-packagestatus) for
the status schema; [the builder guide](../../../tools/pkg/README.md) for node
option syntax.

History: Phase 5, issue #10. Common setup: [README.md](README.md).

## One-time partition-table installation and package build

The separate `brain/` example has a physical `pkg` region (data subtype
`0x43`) at `0x820000`, 8,257,536 bytes through the end of 16 MB flash.
**The supported complete package cap is smaller: `0x780000`, 7,864,320
bytes (7.5 MiB)**, including header, manifest, gaps and payloads. The SOLO
IDU 16 MB layout reserves exactly that supported size before its factory
updater. A BRAIN payload is at most 3 MiB and each NODE at most 1,835,008
bytes. One maximum-size brain plus eight maximum-size nodes does **not** fit.
The host tool and brain reject overflow
([flash layout](../../concepts/flash-layout.md#brain-example-16-mb-brainpartitionscsv)).

Build normally, then **USB-reflash the brain once** with its table. A normal
`/ota` upload alone cannot install that table. Use the actual brain USB port,
and do not flash a node while another task is using it:

```sh
powershell -ExecutionPolicy Bypass -File build.ps1
powershell -ExecutionPolicy Bypass -File build.ps1 examples/bench_app
powershell -ExecutionPolicy Bypass -File build.ps1 brain -p COM63 flash
```

This is the same full brain flash as
[first-flash.md](../../getting-started/first-flash.md#the-brain-brain_ota):
bootloader, table, blank otadata, brain app. It resets brain OTA selection; it
does not erase NVS or the node. Start with no pending package from an earlier
test. Do not add `erase-flash` unless intentionally discarding board data. Use
this package-capable (Phase-5-capable) brain firmware as **both the baseline
and rollback candidate**; pre-Phase-5 firmware has no pending-record handler.
Nodes need the recovery updater/bootloader already installed
([node-recovery.md](node-recovery.md#install-once-over-usb)). Keep the
existing CANH/CANL/GND and GPIO3-to-EN wiring, 250 kbit/s, termination, and
DHCP Ethernet setup.

Build a package from exact unsigned application `.bin` files (no merged images):

```sh
uv run tools/pkg/ecb_pkg.py build --brain brain/build/brain_ota.bin --node examples/bench_app/build/bench_app.bin -o update.ecbpkg
uv run tools/pkg/ecb_pkg.py info update.ecbpkg
```

For a provisioned node, replace its argument with
`--node "examples/bench_app/build/bench_app.bin,cmd=0x701,reply=0x702,name=axis-left"`
([updating-nodes.md](../../guides/updating-nodes.md#1-update-package-via-the-brain)).

## Upload and observe

```sh
curl.exe -i --data-binary "@update.ecbpkg" http://<ip>/package
curl.exe http://<ip>/package/status
curl.exe http://<ip>/node/status
```

202 is sent only after staging/read-back, manifest validation, every payload SHA,
image-envelope check and slot-size check. Large packages take time to validate;
status must stay responsive. No nodes are touched during upload/validation.
After 202 the worker updates/reboots the brain first if needed, then relays NODE
entries in table order. Expect a temporary network interruption during reboot;
poll again after DHCP. `/node/status` remains the status of standalone node jobs;
package progress lives in `/package/status`.

State sequence: `receiving → validating → updating_brain` (if needed), reboot,
`validating → relaying → done`; without a changed brain, skip reboot. A fatal
package/brain failure is `failed`. Node failures do **not** stop later entries:
`done` can contain failed/skipped nodes. Check `brain`, each node's `state` and
`result`, not just `percent:100`. `index` is the zero-based manifest index.
All text is JSON-escaped. Names containing quotes/backslashes must still parse.

Each node is first sent ENTER_UPDATER on the entry's IDs (up to three 0.5 s
attempts; an app with the CAN service ACKs and restarts into its updater, and
the brain waits 1.5 s; an updater NAKs at once). After either answer, INFO uses
four 2 s attempts. Only when ENTER_UPDATER gets no answer, or no INFO follows,
does the four-pulse recovery train and 1 s boot wait run, then another INFO
retry period; an absent node can take over 8 s to fail. **Only the node
physically wired to GPIO3 can be recovered by pulses**; unwired nodes must
already be parked or run an app with the CAN service. A failed probe of an
unwired node can reset the wired one; choose test order accordingly. Each node
`result` names its path: `updater already parked`, `entered updater via CAN`
or `recovered via GPIO3 pulses`.

## Acceptance and fault tests

1. **Same brain skipped.** Use the exact `.bin` just installed over USB (do not
   rebuild it between installation and packaging). Upload with a node entry.
   Expect `brain:skipped_same`, no brain reboot, node `ok` after VERIFY/GO. Compare
   `/` uptime/running slot before/after. Raw SHA includes the ESP appended digest;
   matching version strings alone do not cause a skip.
2. **Changed brain first, then relay.** Build a different normal package-capable
   brain image (e.g. a different `PROJECT_VER` using a separate short build
   directory), package it with a node image and upload. Observe `updating_brain`,
   reboot, self-test VALID, then automatic `relaying` with `brain:updated`. The
   node must see no package CAN transfer before the new brain is VALID. GPIO3 can
   briefly float during the brain reboot
   ([wiring.md](../../getting-started/wiring.md#brain-gpio3--node-en-reset-line));
   that electrical reset is distinct from a CAN update. Example separate image
   build:

   ```sh
   powershell -ExecutionPolicy Bypass -File build.ps1 brain -B p5b -D SDKCONFIG=p5b/sdkconfig -D PROJECT_VER=p5-next app
   uv run tools/pkg/ecb_pkg.py build --brain brain/p5b/brain_ota.bin --node examples/bench_app/build/bench_app.bin -o next.ecbpkg
   curl.exe -i --data-binary "@next.ecbpkg" http://<ip>/package
   ```

3. **Node not parked, automatic recovery.** Let the normal node app run more than
   10 s, without `u` or `/node/recover`. Submit a node-only package. Expect
   `probing → recovering → flashing → ok`; check the existing pulse timings and
   that GPIO3 returns LOW. The new node app should boot and confirm itself. No
   manual serial-port reset is required. Try an absent/unwired ID too: failure
   must be recorded, never success; later entries must still be attempted.
4. **Name mismatch and continuation.** Package the correct IDs with
   `name=definitely-not-this-node`. INFO may recover the node first, but the
   result must be `skipped`, `product name mismatch; node not flashed`, with no
   HELLO/ERASE for it. Follow with an entry with the correct name (or empty name)
   to demonstrate that relay continues. Pre-existing node app contents remain
   unchanged by the mismatched entry.
5. **Corrupt package rejected before changes.** Make a copy and flip the **last
   payload byte**, leaving the manifest/hash unchanged (not an alignment gap):

   ```powershell
   $b = [IO.File]::ReadAllBytes("$PWD\update.ecbpkg")
   $b[$b.Length - 1] = $b[$b.Length - 1] -bxor 1
   [IO.File]::WriteAllBytes("$PWD\corrupt.ecbpkg", $b)
   uv run tools/pkg/ecb_pkg.py info corrupt.ecbpkg
   curl.exe -i --data-binary "@corrupt.ecbpkg" http://<ip>/package
   ```

   Tool exits 2; brain responds 422 with a SHA mismatch. No OTA boot selection,
   node flash, recovery pulse or pending-record update occurs. `pkg` staging
   itself is overwritten. Repeat with a header/table corruption (CRC failure),
   bad version, and an empty body (411). A Content-Length exceeding `pkg` gets
   413 before erase. Also cancel a slow upload: timeout/peer-close must never
   start a relay; reboot returns idle with no retained incomplete upload.
6. **Brain rollback aborts all nodes.** Build the existing skip-mark-valid
   variant from this package-capable source, not an older image:

   ```sh
   powershell -ExecutionPolicy Bypass -File build.ps1 brain -B skip -D SDKCONFIG=skip/sdkconfig -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.skip_mark_valid" app
   uv run tools/pkg/ecb_pkg.py build --brain brain/skip/brain_ota.bin --node examples/bench_app/build/bench_app.bin -o rollback.ecbpkg
   curl.exe -i --data-binary "@rollback.ecbpkg" http://<ip>/package
   ```

   The new brain boots but stays PENDING_VERIFY. Status says the brain is not
   confirmed; no relay or pulses run and mutating jobs stay blocked. Deliberately
   reset/power-cycle the brain (do not accidentally reset it by opening serial).
   The old brain boots VALID, detects a different raw image length/SHA,
   reports `failed`, `brain:rolled_back`, and
   **`brain update rolled back; node relay aborted`**; clears the pending key and
   relays nothing. A new request can then run. A later ordinary reboot resets
   this RAM-only terminal status to idle.
7. **Brain power cut mid-relay.** Start a multi-entry package and cut only brain
   power while a node is flashing. Restore power. The pending key survives;
   after self-test/package revalidation, relay restarts from the **first** node.
   Earlier successful entries may be reflashed. Verify no package resumes from
   an unchecked flash offset; all hashes are rechecked. If a previously flashed
   unwired node now runs an app, it needs manual parking: GPIO3 cannot recover it.
   Also cut before the pending-record commit: no automatic relay is expected.
8. **Shared worker and responsive status.** During slow `/package` receive,
   validation, brain write and node relay, try `/ota`, `/node/scan`,
   `/node/flash`, `/node/recover` and a second `/package`: all must get 409.
   Reverse the conflict with standalone jobs active. `/package/status`,
   `/node/status` and `/` stay responsive (except the actual reboot). Windows
   curl may lose an early reject while uploading a large body; see
   [brain-can-flash.md](brain-can-flash.md#failure-and-isolation-checks-run-only-on-an-available-bench).

A package's payload SHA is an integrity check, not a firmware signature. Tests
must still include bad *internal* ESP image checksum/hash with a recomputed
package SHA: envelope validation can accept it, but brain OTA finalization or
node VERIFY must reject it without selecting that image. A brain failure must
prevent **all** relay; a node failure must leave the old app/updater bootable and
allow subsequent entries to run. Monitor HTTP task stack headroom, watchdogs,
NVS interruption behavior and real GPIO/CAN timing on hardware; host tests and
cross-builds do not prove those properties.
