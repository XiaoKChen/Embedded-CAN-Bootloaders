/* =============================================================================
 * boot_config.h — CAN update path configuration (UNO R4 Minima bootloader)
 * =============================================================================
 * Compile-time constants for the CAN update transport and the handoff magics.
 * Flash/RAM addresses live in memory_map.h, not here.
 *
 * Pure host-compilable header: <stdint.h> only, no target includes.
 * ============================================================================= */

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
#define CAN_BITRATE_BPS     250000u      /* HOCO-tolerance margin (research §5.3, version/c-cpp/ra4m1) */

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
