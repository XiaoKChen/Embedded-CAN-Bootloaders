/* =============================================================================
 * can_update.h — CAN Update Protocol v1 state machine (pure logic)
 * =============================================================================
 * Frame-in / frame-out engine for the flash-over-CAN update path. The engine
 * touches no hardware: the CAN driver feeds it received frames, sends replies
 * it produces on CAN_ID_BOOT_TO_HOST, and flash access goes through the
 * injected HAL — so the whole protocol unit-tests on the host.
 *
 * Protocol v1 (classic CAN, 11-bit IDs, every frame DLC 8, unused bytes 0xFF):
 *
 *   Host -> boot (byte0 = command), required order
 *   HELLO -> SIZE -> ERASE -> DATA... -> VERIFY -> GO:
 *     0x01 HELLO   byte1 = protocol version (0x01)
 *     0x02 ERASE   (erases metadata block FIRST, then the app slot)
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
 *     HELLO ACK: byte3 = bootloader protocol version, bytes4..7 =
 *     APP_SLOT_SIZE uint32 LE. DATA-window ACK: arg = last accepted seq.
 *
 *   Windowing: one window = one 2048-byte flash block (342 DATA frames:
 *   341 x 6 + 1 x 2 bytes) or the final short block. DATA frames inside an
 *   incomplete window get NO reply; the block-completing frame is ACKed after
 *   flash_program succeeds. On an unexpected seq: NAK err 0x01 with arg = the
 *   first seq of the current window; the window buffer is discarded and the
 *   host retransmits the window starting from that seq.
 *
 * Session restart: HELLO is additionally accepted in ANY phase and resets the
 * session (documented deviation for host-retry robustness; every other
 * command is strictly ordered and NAKs err 0x05 out of order).
 *
 * Fail-safe (power loss at any instant):
 *   after ERASE ............. metadata erased -> image invalid -> stay in boot
 *   between DATA blocks ..... metadata still erased -> stay in boot
 *   during VERIFY metadata
 *   write ................... metadata torn/partial -> header CRC fails ->
 *                             stay in boot
 *   after VERIFY ............ new image fully validated -> boots
 * There is no state that bricks: the bootloader region is never written.
 * ============================================================================= */

#ifndef CAN_UPDATE_H
#define CAN_UPDATE_H

#include <stdbool.h>
#include <stdint.h>

#include "boot_config.h"
#include "memory_map.h"

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

#define CAN_UPDATE_REPLY_MARKER      0x79u  /* byte0 of every ACK/NAK          */
#define CAN_UPDATE_OK                0x00u  /* byte2 of an ACK                 */
#define CAN_UPDATE_ERR_SEQ           0x01u  /* unexpected seq; arg = expected  */
#define CAN_UPDATE_ERR_FLASH         0x02u  /* program/erase/read-back failure */
#define CAN_UPDATE_ERR_CRC           0x03u  /* VERIFY CRC mismatch             */
#define CAN_UPDATE_ERR_SIZE          0x04u  /* SIZE zero or > APP_SLOT_SIZE    */
#define CAN_UPDATE_ERR_STATE         0x05u  /* out-of-order/unknown/malformed  */

#define CAN_UPDATE_DATA_BYTES_PER_FRAME 6u
#define CAN_UPDATE_FILL_BYTE            0xFFu

/* -----------------------------------------------------------------------------
 * Flash HAL seam — the only way the engine reaches hardware
 * ----------------------------------------------------------------------------- */
typedef struct {
    /* Erase the metadata block FIRST (invalidates the old image before any
     * app byte is disturbed — fail closed), then the whole app slot. */
    bool (*flash_erase_app_and_metadata)(void);

    /* Program `len` bytes (multiple of FLASH_WRITE_UNIT) at absolute flash
     * address `addr`. The implementation performs write + read-back verify
     * and returns false on any failure. */
    bool (*flash_program)(uint32_t addr, const uint8_t *data, uint32_t len);

    /* Code flash as a readable byte buffer (VERIFY reads back through it). */
    const uint8_t *flash_base;
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
    CAN_UPDATE_PHASE_VERIFIED,     /* metadata written; awaiting GO            */
} can_update_phase_t;

typedef enum {
    CAN_UPDATE_ACTION_NONE = 0,
    CAN_UPDATE_ACTION_RESET,       /* GO accepted: caller performs the reset   */
} can_update_action_t;

typedef struct {
    const can_update_hal_t *hal;
    can_update_phase_t phase;
    uint32_t image_size;           /* from SIZE                                */
    uint32_t bytes_received;       /* image bytes accepted so far              */
    uint32_t block_index;          /* next/current 2048-byte block             */
    uint32_t block_fill;           /* bytes buffered for the current block     */
    uint8_t  expected_seq;         /* seq of the next DATA frame               */
    uint8_t  window_start_seq;     /* first seq of the current window          */
    uint8_t  block_buf[FLASH_BLOCK_SIZE];
} can_update_state_t;

/* -----------------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------------- */
void can_update_init(can_update_state_t *st, const can_update_hal_t *hal);

/**
 * Feed one received CAN frame to the engine.
 *
 * @param st          Engine state.
 * @param id          Received CAN ID; frames not addressed to
 *                    CAN_ID_HOST_TO_BOOT are silently ignored.
 * @param dlc         Received DLC; protocol frames must be DLC 8.
 * @param data        8 payload bytes.
 * @param reply_data  Out: 8-byte reply payload for CAN_ID_BOOT_TO_HOST.
 * @param out_action  Out: CAN_UPDATE_ACTION_RESET after an accepted GO.
 * @return true when reply_data holds a reply to transmit; false otherwise.
 */
bool can_update_handle_frame(can_update_state_t *st, uint32_t id, uint8_t dlc,
                             const uint8_t data[8], uint8_t reply_data[8],
                             can_update_action_t *out_action);

#ifdef __cplusplus
}
#endif

#endif /* CAN_UPDATE_H */
