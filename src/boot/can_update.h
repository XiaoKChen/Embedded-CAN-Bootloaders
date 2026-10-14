/* =============================================================================
 * can_update.h — CAN Update Protocol v1 state machine (pure logic)
 * =============================================================================
 * Frame-in / frame-out engine for the flash-over-CAN update path. The engine
 * touches no hardware: the CAN driver feeds it received frames, sends replies
 * it produces on the boot -> host reply ID, and every access to the update
 * slot goes through the injected slot HAL — so the whole protocol unit-tests
 * on the host and the same engine serves any target (ESP32-S3: the HAL wraps
 * esp_ota_begin/write/end on the inactive OTA partition).
 *
 * Per-device IDs: the host -> boot command ID the engine matches on is a
 * runtime value supplied to can_update_init (default CAN_ID_HOST_TO_BOOT); the
 * caller transmits every reply on the paired boot -> host ID (default
 * CAN_ID_BOOT_TO_HOST). This lets several parked boards share one bus, each
 * addressed on its own ID (see dfu_config.h v2 config block).
 *
 * Protocol v1 (classic CAN, 11-bit IDs, every frame DLC 8, unused bytes 0xFF):
 *
 *   Host -> boot (byte0 = command), required order
 *   HELLO -> SIZE -> ERASE -> DATA... -> VERIFY -> GO:
 *     0x01 HELLO   byte1 = protocol version (0x01)
 *     0x02 ERASE   prepare the update slot (never touches the boot image)
 *     0x03 DATA    byte1 = seq (uint8, starts at 0, mod 256 across the whole
 *                  transfer), bytes2..7 = 6 image bytes (final frame may carry
 *                  fewer valid bytes — the receiver counts against image_size)
 *     0x04 SIZE    bytes2..5 = image_size, uint32 LE
 *     0x05 VERIFY  bytes2..5 = expected image CRC32, uint32 LE
 *     0x06 GO
 *
 *   Boot -> host on CAN_ID_BOOT_TO_HOST:
 *     ACK  {0x79, cmd_echo, 0x00, arg, 0xFF, 0xFF, 0xFF, 0xFF}
 *     NAK  {0x79, cmd_echo, err,  arg, 0xFF, 0xFF, 0xFF, 0xFF}
 *     HELLO ACK: byte3 = bootloader protocol version, bytes4..7 = the slot
 *     size (hal->slot_size) uint32 LE. DATA-window ACK: arg = last accepted
 *     seq.
 *
 *   Windowing: one window = CAN_UPDATE_WINDOW_SIZE (2048) image bytes (342
 *   DATA frames: 341 x 6 + 1 x 2 bytes) or the final short window. DATA frames
 *   inside an incomplete window get NO reply; the window-completing frame is
 *   ACKed after slot_program succeeds. On an unexpected seq: NAK err 0x01 with
 *   arg = the first seq of the current window; the window buffer is discarded
 *   and the host retransmits the window starting from that seq. Window start
 *   seqs repeat every 128 windows, so a host resolves a rewind against its
 *   current window and the next one only (a slot > 256 KB spans > 128).
 *
 * Session restart: HELLO is additionally accepted in ANY phase and resets the
 * session (documented deviation for host-retry robustness; every other
 * command is strictly ordered and NAKs err 0x05 out of order).
 *
 * Fail-safe (power loss at any instant) — the HAL contract below guarantees
 * the slot being written is never the image the device boots before GO:
 *   during ERASE / DATA ..... slot partial, not selected for boot -> the
 *                             device comes back up in the updater
 *   after VERIFY, before GO . image accepted but not selected -> updater
 *   GO (slot_activate) ...... the target's atomic boot-selection write; the
 *                             new image boots only once it has completed
 * There is no state that bricks: the engine never writes the updater itself.
 * ============================================================================= */

#ifndef CAN_UPDATE_H
#define CAN_UPDATE_H

#include <stdbool.h>
#include <stdint.h>

#include "boot_config.h"  /* default CAN IDs (see the header comment) */

#ifdef __cplusplus
extern "C" {
#endif

/* -----------------------------------------------------------------------------
 * Protocol constants
 * ----------------------------------------------------------------------------- */
#define CAN_UPDATE_PROTOCOL_VERSION  0x01u

#define CAN_UPDATE_CMD_HELLO         0x01u
#define CAN_UPDATE_CMD_ERASE         0x02u
#define CAN_UPDATE_CMD_DATA          0x03u
#define CAN_UPDATE_CMD_SIZE          0x04u
#define CAN_UPDATE_CMD_VERIFY        0x05u
#define CAN_UPDATE_CMD_GO            0x06u
/* INFO is answered by the bootloader integration layer (main.c CAN service),
 * NOT by this engine: its reply is 5 frames, which the single-reply engine
 * cannot express. The constant is reserved here so main and the engine agree
 * on 0x07; if a frame with this command ever reaches the engine it falls
 * through to the default case and is NAKed as an unknown command. */
#define CAN_UPDATE_CMD_INFO          0x07u

#define CAN_UPDATE_REPLY_MARKER      0x79u  /* byte0 of every ACK/NAK          */
#define CAN_UPDATE_OK                0x00u  /* byte2 of an ACK                 */
#define CAN_UPDATE_ERR_SEQ           0x01u  /* unexpected seq; arg = expected  */
#define CAN_UPDATE_ERR_FLASH         0x02u  /* program/erase/read-back failure */
#define CAN_UPDATE_ERR_CRC           0x03u  /* VERIFY CRC mismatch             */
#define CAN_UPDATE_ERR_SIZE          0x04u  /* SIZE zero or > slot size        */
#define CAN_UPDATE_ERR_STATE         0x05u  /* out-of-order/unknown/malformed  */
#define CAN_UPDATE_ERR_IMAGE         0x06u  /* CRC ok, but the target rejected
                                               the image (ESP32: esp_ota_end
                                               format/chip/SHA-256 checks)     */

#define CAN_UPDATE_DATA_BYTES_PER_FRAME 6u
#define CAN_UPDATE_FILL_BYTE            0xFFu
#define CAN_UPDATE_WINDOW_SIZE          2048u  /* image bytes per DATA window;
                                                  frozen protocol constant    */

/* -----------------------------------------------------------------------------
 * Slot HAL seam — the only way the engine reaches hardware
 * -----------------------------------------------------------------------------
 * Offsets are relative to the start of the update slot. The contract every
 * implementation keeps (it is what makes the fail-safe table hold): the slot
 * written here is never the image the device boots until slot_activate has
 * returned true.
 */
typedef struct {
    /* Bytes the slot can hold: announced in the HELLO ACK, bounds SIZE. */
    uint32_t slot_size;

    /* ERASE: start a fresh write session on the slot, abandoning any earlier
     * one. May defer the physical erase into slot_program. */
    bool (*slot_erase)(void);

    /* Write `len` bytes (one window, <= CAN_UPDATE_WINDOW_SIZE) at `offset`.
     * The engine calls it in ascending window order and retries the same
     * offset after a failure. The implementation pads to its flash write unit
     * itself, performs write + read-back verify, and returns false on any
     * failure (including a retry it cannot honour). */
    bool (*slot_program)(uint32_t offset, const uint8_t *data, uint32_t len);

    /* Read `len` bytes back from `offset` (VERIFY CRCs the slot through it). */
    bool (*slot_read)(uint32_t offset, uint8_t *buf, uint32_t len);

    /* VERIFY, once the CRC matched: the target's own acceptance of the image
     * (format / signature checks). false -> NAK CAN_UPDATE_ERR_IMAGE. */
    bool (*slot_commit)(void);

    /* GO: make the verified slot the boot image. false -> NAK
     * CAN_UPDATE_ERR_FLASH and the device stays in the updater. */
    bool (*slot_activate)(void);
} can_update_hal_t;

/* -----------------------------------------------------------------------------
 * Engine state — statically allocatable, no heap
 * ----------------------------------------------------------------------------- */
typedef enum {
    CAN_UPDATE_PHASE_IDLE = 0,     /* awaiting HELLO                           */
    CAN_UPDATE_PHASE_GREETED,      /* awaiting SIZE                            */
    CAN_UPDATE_PHASE_SIZED,        /* awaiting ERASE                           */
    CAN_UPDATE_PHASE_TRANSFER,     /* awaiting DATA                            */
    CAN_UPDATE_PHASE_TRANSFERRED,  /* all image bytes programmed; await VERIFY */
    CAN_UPDATE_PHASE_VERIFIED,     /* CRC + slot_commit passed; awaiting GO    */
} can_update_phase_t;

typedef enum {
    CAN_UPDATE_ACTION_NONE = 0,
    CAN_UPDATE_ACTION_RESET,       /* GO accepted, slot activated: the caller
                                      performs the reset                      */
} can_update_action_t;

typedef struct {
    const can_update_hal_t *hal;
    uint32_t cmd_id;               /* host -> boot ID this engine answers      */
    can_update_phase_t phase;
    uint32_t image_size;           /* from SIZE                                */
    uint32_t bytes_received;       /* image bytes accepted so far              */
    uint32_t block_index;          /* next/current window                      */
    uint32_t block_fill;           /* bytes buffered for the current window    */
    uint8_t  expected_seq;         /* seq of the next DATA frame               */
    uint8_t  window_start_seq;     /* first seq of the current window          */
    uint8_t  block_buf[CAN_UPDATE_WINDOW_SIZE];  /* window; VERIFY read buffer */
} can_update_state_t;

/* -----------------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------------- */
void can_update_init(can_update_state_t *st, const can_update_hal_t *hal,
                     uint32_t cmd_id);

/**
 * Feed one received CAN frame to the engine.
 *
 * @param st          Engine state.
 * @param id          Received CAN ID; frames not addressed to the engine's
 *                    configured cmd_id (see can_update_init) are silently
 *                    ignored.
 * @param dlc         Received DLC; protocol frames must be DLC 8.
 * @param data        8 payload bytes.
 * @param reply_data  Out: 8-byte reply payload for CAN_ID_BOOT_TO_HOST.
 * @param out_action  Out: CAN_UPDATE_ACTION_RESET after an accepted GO (the
 *                    slot is already activated; the caller only resets).
 * @return true when reply_data holds a reply to transmit; false otherwise.
 */
bool can_update_handle_frame(can_update_state_t *st, uint32_t id, uint8_t dlc,
                             const uint8_t data[8], uint8_t reply_data[8],
                             can_update_action_t *out_action);

#ifdef __cplusplus
}
#endif

#endif /* CAN_UPDATE_H */
