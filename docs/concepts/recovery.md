# Recovery: how a node gets into its updater

**No recovery pin.** ESP-IDF's GPIO factory reset
(`CONFIG_BOOTLOADER_FACTORY_RESET`) is off: GPIO4 is the node's TWAI TX, and
the bootloader would sample the transceiver's D line at reset. Instead, the
node bootloader (`node_updater/bootloader_components/main`) implements
software recovery: an RTC crash-loop counter, plus a flash boot counter the
brain can trip with EN pulses over its existing GPIO3-to-EN inverting stage.
No PCB change, signing, or CAN protocol change. History: #9.

## Entry paths

| Path | Mechanism |
|---|---|
| **CAN request** | ENTER_UPDATER (`0x09`) on the node's own IDs, from the brain's relay, `/node/flash` or `can_flash.py --enter-updater`: an app with `examples/node_app`'s CAN service ACKs, then calls `can_updater_client_enter_updater()`. History: #11 |
| **App request** (`can_flash.py --enter-bootloader`, the bench app's `u`) | the app calls `can_updater_client_enter_updater()` (`components/can_updater_client`), which selects `factory` and restarts — the ESP32 twin of the RA4M1 app writing the handoff magic and resetting |
| **No valid app** | fresh board, both slots invalid, or an app that was rolled back because it never called `esp_ota_mark_app_valid_cancel_rollback()` |
| **Software recovery** | three unclean SW/panic/WDT resets in RTC, or three power-on/EN boots in a flash bit log, without 10 seconds of stable uptime, force `factory` without changing `otadata`. Trips remain latched until 10 s of updater uptime or a verified GO. Normal `esp_restart()` is marked clean by the client shutdown handler |
| **Brain-forced recovery** | `POST /node/recover`, or a package's automatic recovery: four pulses, each **GPIO3 HIGH 100 ms** (node held reset), then **LOW 400 ms** (node runs), finally leaving LOW — three power-on/EN boots trip the flash counter. Only the node physically wired to GPIO3 |
| **USB fallback** | erase only `otadata`: `python -m esptool --chip esp32s3 erase_region 0xe000 0x2000` (add `-p <port>`); factory then remains selected until another image is flashed |

```sh
python -m esptool --chip esp32s3 -p COM7 erase_region 0xe000 0x2000
```

Hosts and the brain try the CAN request first; GPIO3 pulses run only when
nothing answers ([updating-nodes.md](../guides/updating-nodes.md)).

## Parked behaviour and stable clear

The updater always parks waiting for the host. **App-requested** entry erases
`otadata` and remains parked across resets (until an image is flashed).
**Counter-forced** entry preserves `otadata`: after 10 s the counters clear
but the updater keeps running; a later reset may boot the previously selected
app. Re-run recovery if a failed transfer after clear returns there. Forced
recovery is not permanent app disabling. A verified GO also clears both
counters before selecting the new image, so recovery can be followed
immediately by a flash; no 10 s wait is needed.

- Both counts saturate at 3. RTC trips also latch flash so EN cannot release
  them. Only the updater may clear a trip, after 10 s or after successful
  VERIFY immediately before GO. Normal apps clear only untripped evidence.
- The 10 s timer starts at the app's early health call
  (`can_updater_client_start_health()`), not power-on or CAN discovery, and
  clears once after 10 s of uninterrupted uptime
  (`node_health: 10 s stable uptime: recovery counters cleared`). An already
  erased sector is not erased again.
- Deliberate `esp_restart()` does not increment the RTC crash count. Its
  clean mark is consumed on the next software reset only, never on a
  hardware WDT reset.
- Three rapid ordinary power cycles intentionally look like a recovery request.
- After VERIFY, GO clears and verifies both counters before changing otadata
  and restarting. Clear failure rejects GO and leaves the updater running.
- Garbage in bootcnt is treated as zero unless it contains at least three
  exact valid records.
- From a healthy erased log, release 3 of the four-pulse train must select
  factory, and release 4 must still select factory. A genuinely torn write is
  ignored; one lost record can defer the third valid record until release 4.
  Persistent flash faults cannot be repaired by counting uncertain bits as
  evidence. The node records before app validation/loading, with no
  intentional delay and normally no erase.

Apps must call `can_updater_client_start_health()` once early in `app_main`
and reserve the same 16 custom RTC bytes as `node_updater/sdkconfig.defaults`
([writing-a-node-app.md](../guides/writing-a-node-app.md#recovery-contract)).
Rebuild older apps for clean-restart and stable-clear support. Existing nodes
need a one-time USB bootloader/table/updater flash; CAN cannot replace the
bootloader ([install once](../testing/bench/node-recovery.md#install-once-over-usb)).

## Fail-safe and power loss

Nothing in the update path writes the `factory` partition. The only
boot-selection change is the `otadata` write on GO, which ESP-IDF makes atomic
across its two sectors. With app-requested factory entry, power loss before
GO comes back in the updater. With counter-forced entry, after stable clear
the prior OTA selection remains the fallback; re-trigger recovery if needed.
After GO the new app boots in `PENDING_VERIFY`; failure to confirm rolls back
to the previous valid image (or factory when none exists). Full table:
`node_updater/main/main.c`.

Bootloader flash mutation / interruption outcomes:

| Operation | Interrupted by EN/power loss |
|---|---|
| Append 16 bytes using only 1→0 bits | Exact record counts; torn/weak records are ignored and their slots skipped. Read-back failure logs a warning and continues boot. No image is touched. |
| Full **untripped** log wrap | Only after all 256 slots are consumed: erase, verify, restore prior count plus this boot. Interruption may lose evidence, intentionally failing toward normal boot. A tripped log never wraps. |
| Stable clear / verified GO clear (one 4 KB erase) | After 10 s, or after successful VERIFY before GO. Partial erase leaves zero/some exact old records; later boots remain latched or use the prior selection. RTC clears only after flash clear verifies. Failed GO clear rejects GO without changing otadata. |
| RTC/clean-mark update | Magic, bounded fields and check value reject garbage/interruption. Invalid state starts at zero (the current unclean reset can then count once). A flash latch remains authoritative. |

## Flash wear

The flash sector contains 256 independently recognizable 128-bit records.
Three valid records trip; further rapid boots write nothing. Normal wear is
**one sector erase per stable power-on session**, zero per clean SW restart
when already empty. At an assumed conservative 10,000 erase endurance, one
stable power cycle/day is about 27 years, ten/day about 2.7 years; check the
actual in-package flash specification. Frequent stable power cycling, not
a four-pulse burst, dominates wear. A bad/unreadable region never blocks boot.

## Implementation notes

IDF v5.3.6 hooks cannot select the partition, so
`node_updater/bootloader_components/main` overrides startup but retains IDF's
hardware init, linker, image verification/loading and normal OTA rollback.
Recovery chooses factory **before** normal OTA selection can change pending
metadata. The 16-byte RTC record uses IDF's reserved FAST region, outside
its CRC. IDF's independent retained-state CRC is established first so its
image-loader update cannot zero the newly written custom state.

Verified in pinned IDF source: `esp_system.c:esp_restart` runs shutdown
handlers; `port/panic_handler.c:panic_restart` directly calls
`esp_restart_noos`; hardware WDT resets execute no application shutdown
code. S3 raw reason `0x01` aliases power-on, chip brownout and super-WDT,
which are indistinguishable and all use the flash path.

Procedures and fault tests: [bench/node-recovery.md](../testing/bench/node-recovery.md).
