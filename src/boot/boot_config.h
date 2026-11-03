/**
 * @file    boot_config.h
 * @brief   CAN update path configuration (UNO R4 Minima bootloader).
 *
 * Compile-time constants for the CAN update transport (IDs, bitrate, CAN
 * update mode idle timeout) and the handoff magics.
 *
 * Owns:         The default CAN update IDs and bitrate, the CAN update mode
 *               idle timeout, the parked loop's CAN re-init interval, the
 *               CAN-update handoff magic and the image metadata magic.
 * Not here:     Flash/RAM addresses (memory_map.h); per-device ID overrides
 *               (dfu_config.h).
 * Depends on:   <stdint.h> only: a pure host-compilable header, no target
 *               includes.
 *
 * @see     memory_map.h, dfu_config.h, can_update.h
 */

#ifndef BOOT_CONFIG_H
#define BOOT_CONFIG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -----------------------------------------------------------------------------
 * CAN bus (classic CAN 2.0B, 11-bit IDs, DLC 8 on every protocol frame)
 * ----------------------------------------------------------------------------- */
/* DEFAULT per-device CAN update IDs. A v2 DFU config block (dfu_config.h) can
 * override them per board so several parked boards share one bus; an
 * unprovisioned board, a v1 block, or an out-of-range field falls back to these
 * (dfu_config_can_ids()). The engine and the CAN service take the effective IDs
 * at runtime — these constants are only the fallback. */
#define CAN_ID_HOST_TO_BOOT 0x79Eu       /* default host -> bootloader commands */
#define CAN_ID_BOOT_TO_HOST 0x79Fu       /* default bootloader -> host ACK/NAK  */
#define CAN_BITRATE_BPS     250000u      /* HOCO-tolerance margin (research §5.3) */

/* CAN update mode (VBTBKR[4] entry) resets after this long without an
 * accepted update-protocol frame (can_update_accepts_frame: command ID,
 * DLC 8, command byte HELLO..INFO, 0xFF padding; INFO counts only once a
 * session is past IDLE); other traffic on that ID, such as the master's app
 * polls, does not count. Before ERASE the old app
 * boots again, after ERASE the update-in-progress marker parks the board in
 * DFU + CAN. The parked DFU + CAN loop has no timeout.
 *
 * Must exceed the host flasher's longest silent gap mid-session:
 *   - ordinary commands: 2 s ACK timeout x 4 attempts, ~8 s;
 *   - ERASE: the board is busy erasing for a few seconds and the timer
 *     restarts only once ERASE has been handled. If that ACK is lost, the
 *     host resends ERASE after its own ERASE timeout (15 s in can_flash.py
 *     and the Airform flasher, counted from its send) plus its retry gap,
 *     and the resend is re-ACKed without a second erase. 20 s keeps that
 *     resend inside this timeout with margin even if the erase were
 *     instant. Must stay strictly above the hosts' ERASE timeout plus a
 *     retry gap.
 * The parked DFU + CAN loop uses the same bound as the ownership timeout of
 * a stalled CAN session or DFU download (main.c flash_ownership_timeouts). */
#define CAN_UPDATE_IDLE_TIMEOUT_MS 20000u

/* When CAN is down (init failed, or the driver reports a failed bus-off
 * recovery), the parked DFU + CAN loop retries can_polled_init() at most this
 * often; CAN update mode resets instead. */
#define CAN_REINIT_INTERVAL_MS 1000u

/* -----------------------------------------------------------------------------
 * Handoff & image magics
 * ----------------------------------------------------------------------------- */
/* Written by the application to R_SYSTEM->VBTBKR[4] (PRCR-unlocked) before a
 * reset to request CAN update mode; the bootloader clears it on entry.
 * VBTBKR[0] stays reserved for the stock DFU double-tap magic 0x07738135.   */
#define CAN_UPDATE_MAGIC    0xB007CAFEu

/* Magic of the image metadata block at METADATA_ADDR (see image_header.h). */
#define IMAGE_META_MAGIC    0xB007D00Du

#ifdef __cplusplus
}
#endif

#endif /* BOOT_CONFIG_H */
