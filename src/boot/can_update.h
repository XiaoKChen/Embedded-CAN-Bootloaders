/**
 * @file    can_update.h
 * @brief   CAN Update Protocol v1 state machine (pure logic).
 *
 * Frame-in / frame-out engine for the flash-over-CAN update path. The engine
 * touches no hardware: the CAN driver feeds it received frames, sends replies
 * it produces on the boot -> host reply ID, and flash access goes through the
 * injected HAL — so the whole protocol unit-tests on the host.
 *
 * Protocol v1 (classic CAN, 11-bit IDs, every frame DLC 8, unused bytes 0xFF):
 *
 *   Host -> boot (byte0 = command), required order
 *   HELLO -> SIZE -> ERASE -> DATA... -> VERIFY -> GO:
 *     0x01 HELLO   byte1 = protocol version (0x01)
 *     0x02 ERASE   (puts the update-in-progress marker in place FIRST, then
 *                  erases the app slot)
 *     0x03 DATA    byte1 = seq (uint8, starts at 0, mod 256 across the whole
 *                  transfer), bytes2..7 = 6 image bytes (final frame may carry
 *                  fewer valid bytes — the receiver counts against image_size)
 *     0x04 SIZE    bytes2..5 = image_size, uint32 LE (8 .. APP_SLOT_SIZE)
 *     0x05 VERIFY  bytes2..5 = expected image CRC32, uint32 LE
 *     0x06 GO
 *
 *   Accepted frames (can_update_accepts_frame): on the command ID, DLC 8,
 *   command byte 0x01..0x07, and every unused byte 0xFF (HELLO bytes 2..7;
 *   SIZE/VERIFY bytes 1, 6, 7; ERASE/GO/INFO bytes 1..7). DATA carries
 *   payload, so it is accepted only while a transfer is in progress, or
 *   once it is complete (TRANSFERRED) with a seq of the last window (see
 *   Host retries): otherwise a 0x03-led frame is a 3-byte ISO-TP single
 *   frame from the app protocol, not DATA. Anything else, including the 0x79 reply
 *   marker and ISO-TP traffic sharing the command ID, gets NO reply. Every
 *   known host (tools/can-flash, the Airform flasher, the ESP32 brain's
 *   can_flash_host) pads this way. The padding is the only thing that tells
 *   commands from app traffic on a shared ID, so the app protocol must not
 *   pad with 0xFF: e.g. {0x01, x, 0xFF x 6} is a HELLO for any x (byte 1 is
 *   the version, not padding), which restarts a session or draws a NAK.
 *
 *   Boot -> host on the device's reply ID (default CAN_ID_BOOT_TO_HOST):
 *     ACK  {0x79, cmd_echo, 0x00, arg, 0xFF, 0xFF, 0xFF, 0xFF}
 *     NAK  {0x79, cmd_echo, err,  arg, 0xFF, 0xFF, 0xFF, 0xFF}
 *     HELLO ACK: byte3 = bootloader protocol version, bytes4..7 =
 *     APP_SLOT_SIZE uint32 LE. DATA-window ACK: arg = last accepted seq.
 *
 *   Windowing: one window = one 2048-byte flash block (342 DATA frames:
 *   341 x 6 + 1 x 2 bytes) or the final short block. DATA frames inside an
 *   incomplete window get NO reply; the block-completing frame is ACKed after
 *   flash_program succeeds. Block 0's first 8 bytes (the vector unit) are
 *   held in RAM and programmed by VERIFY, just before the record. On an unexpected seq: NAK err 0x01 with arg = the
 *   first seq of the current window; the window buffer is discarded and the
 *   host retransmits the window starting from that seq.
 *
 * Session restart: HELLO is additionally accepted in ANY phase and resets the
 * session (documented deviation for host-retry robustness; every other
 * command is strictly ordered and NAKs err 0x05 out of order).
 *
 * Host retries: a host resends SIZE/ERASE/VERIFY when the ACK is lost, so an
 * exact repeat of the previous command is ACKed again without redoing it:
 * SIZE with the same size while SIZED, ERASE while TRANSFER before any DATA,
 * VERIFY with the committed CRC while VERIFIED. A host resends a DATA window
 * whose ACK was lost; for the final window (phase TRANSFERRED) nothing is
 * programmed again: its frames are accepted silently and the final-seq frame
 * is re-ACKed (arg = final seq). For a full 342-frame final window the final
 * seq also occurs 256 frames earlier, so the re-ACK comes at that frame; the
 * host then simply moves on to VERIFY. A DATA or VERIFY flash
 * failure (err 0x02) drops the session to IDLE: the slot is no longer known
 * to be erased, so the host must restart from HELLO.
 *
 * Fail-safe (power loss at any instant; outcomes per boot_decide in
 * image_validate.h, marker layout in image_header.h, ERASE's first steps per
 * update_start_plan):
 *   before ERASE ............ nothing written -> as before the session (the
 *                             old app boots, or the board parks)
 *   ERASE retrying an
 *   interrupted update ...... marker set and record area blank: the
 *                             metadata block is not erased -> stay in boot
 *   ERASE, slot not
 *   bootable ................ the APP_BASE block is erased BEFORE the
 *                             metadata block -> parks throughout: the SP
 *                             fallback later finds an erased SP word
 *   ERASE, slot bootable,
 *   before the marker is
 *   programmed .............. record erased/torn, marker absent/torn (cannot
 *                             match), app slot untouched -> SP-word fallback
 *                             boots the old, intact app. Exception: a marker
 *                             left by the previous, completed update can
 *                             survive a torn metadata erase that took the
 *                             record -> parks until an update completes
 *   rest of ERASE, between
 *   DATA blocks ............. marker present, no record -> stay in boot
 *                             (DFU + CAN). Block 0 is programmed from its
 *                             second write unit: the vector unit (SP + reset
 *                             vector) stays erased, so even a lost marker
 *                             cannot take the SP fallback into the image
 *   during VERIFY vector-unit
 *   write ................... only after the CRC (vector unit from RAM, the
 *                             rest from flash) and the vector bounds pass;
 *                             no record, marker present -> stay in boot
 *   during VERIFY metadata
 *   write ................... record torn/partial -> header CRC fails,
 *                             marker present -> stay in boot
 *   after VERIFY ............ new image fully validated -> boots
 * Link loss with power on: CAN update mode (main.c) resets after
 * CAN_UPDATE_IDLE_TIMEOUT_MS without a command frame, landing in the row
 * above that matches how far the session got.
 * There is no state that bricks: the bootloader region is never written.
 *
 * Owns:         The protocol constants (below), the session phases and window
 *               buffer (can_update_state_t) and the flash HAL contract
 *               (can_update_hal_t).
 * Not here:     INFO, answered by the integration layer (main.c CAN service);
 *               the CAN driver (can_polled.h); the reset after GO, the idle
 *               timeout and the ownership timeout (the caller, main.c, via
 *               can_update_end_session); which transport owns flash (the
 *               caller, through hal->flash_locked).
 * Invariants:   Per-device IDs: the host -> boot command ID the engine
 *               matches on is a runtime value supplied to can_update_init
 *               (default CAN_ID_HOST_TO_BOOT); the caller transmits every
 *               reply on the paired boot -> host ID (default
 *               CAN_ID_BOOT_TO_HOST). This lets several parked boards share
 *               one bus, each addressed on its own ID (see dfu_config.h v2
 *               config block).
 * Errors:       A rejected protocol frame is answered with a NAK carrying a
 *               CAN_UPDATE_ERR_* code; frames that are not protocol frames
 *               (see above) get no reply.
 * Depends on:   boot_config.h, memory_map.h, image_header.h and
 *               image_validate.h. No hardware, no heap.
 *
 * @see     can_update.c, test_can_update.c, image_validate.h, image_header.h
 */

#ifndef CAN_UPDATE_H
#define CAN_UPDATE_H

#include <stdbool.h>
#include <stdint.h>

#include "boot_config.h"
#include "image_header.h"
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
/* INFO is answered by the bootloader integration layer (main.c CAN service),
 * NOT by this engine: its reply is 5 frames, which the single-reply engine
 * cannot express. The constant is reserved here so main and the engine agree
 * on 0x07; the engine itself never replies to it. */
#define CAN_UPDATE_CMD_INFO          0x07u

#define CAN_UPDATE_REPLY_MARKER      0x79u  /* byte0 of every ACK/NAK          */
#define CAN_UPDATE_OK                0x00u  /* byte2 of an ACK                 */
#define CAN_UPDATE_ERR_SEQ           0x01u  /* unexpected seq; arg = expected  */
#define CAN_UPDATE_ERR_FLASH         0x02u  /* program/erase/read-back failure */
#define CAN_UPDATE_ERR_CRC           0x03u  /* VERIFY: CRC mismatch or bad
                                               SP/entry vector words           */
#define CAN_UPDATE_ERR_SIZE          0x04u  /* SIZE < 8 or > APP_SLOT_SIZE     */
#define CAN_UPDATE_ERR_STATE         0x05u  /* out of order, bad HELLO version,
                                               or flash owned by USB DFU       */

#define CAN_UPDATE_DATA_BYTES_PER_FRAME 6u
#define CAN_UPDATE_FILL_BYTE            0xFFu

/* -----------------------------------------------------------------------------
 * Flash HAL seam — the only way the engine reaches hardware
 * ----------------------------------------------------------------------------- */
typedef struct {
    /* Put the update-in-progress marker in place FIRST, erasing what
     * update_start_plan (image_validate.h) requires (an interrupted update
     * then parks instead of booting a partial image — fail closed), then
     * erase the whole app slot. */
    bool (*flash_erase_app_and_metadata)(void);

    /* Program `len` bytes (multiple of FLASH_WRITE_UNIT) at absolute flash
     * address `addr`. The implementation performs write + read-back verify
     * and returns false on any failure. */
    bool (*flash_program)(uint32_t addr, const uint8_t *data, uint32_t len);

    /* Code flash as a readable byte buffer (VERIFY reads back through it). */
    const uint8_t *flash_base;

    /* True while another transport (the USB DFU download) owns flash:
     * SIZE, ERASE, DATA and VERIFY then NAK err 0x05 without touching flash
     * or claiming it. */
    bool (*flash_locked)(void);
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
    /* First members: aligned for the flash program source without padding. */
    _Alignas(FLASH_WRITE_UNIT) uint8_t block_buf[FLASH_BLOCK_SIZE];
    /* The image's vector unit, held from block 0 until VERIFY commits it. */
    _Alignas(FLASH_WRITE_UNIT) uint8_t vector_unit[IMAGE_VECTOR_UNIT_SIZE];
    const can_update_hal_t *hal;
    uint32_t cmd_id;               /* host -> boot ID this engine answers      */
    can_update_phase_t phase;
    uint32_t image_size;           /* from SIZE                                */
    uint32_t bytes_received;       /* image bytes accepted so far              */
    uint32_t block_index;          /* next/current 2048-byte block             */
    uint32_t block_fill;           /* bytes buffered for the current block     */
    uint8_t  expected_seq;         /* seq of the next DATA frame               */
    uint8_t  window_start_seq;     /* first seq of the current window (the
                                      last one once TRANSFERRED)               */
} can_update_state_t;

/* -----------------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------------- */
void can_update_init(can_update_state_t *st, const can_update_hal_t *hal,
                     uint32_t cmd_id);

/**
 * True when a received frame is an update-protocol frame for this engine
 * (rules in the file header). The caller uses it to pick INFO out and to
 * restart its idle timer; can_update_handle_frame ignores everything else.
 */
bool can_update_accepts_frame(const can_update_state_t *st, uint32_t id,
                              uint8_t dlc, const uint8_t data[8]);

/**
 * True from an accepted SIZE until the session returns to IDLE/GREETED (HELLO,
 * a flash failure, or the reset after GO): the CAN session has claimed flash
 * and the USB DFU download must not start.
 */
bool can_update_owns_flash(const can_update_state_t *st);

/**
 * Drop the session to IDLE without touching flash: the caller's ownership
 * timeout for a host that went silent. Whatever ERASE/DATA did stays, and
 * the update-in-progress marker keeps the board parked; the host restarts
 * from HELLO.
 */
void can_update_end_session(can_update_state_t *st);

/**
 * Feed one received CAN frame to the engine.
 *
 * @param st          Engine state.
 * @param id          Received CAN ID.
 * @param dlc         Received DLC. Frames can_update_accepts_frame rejects
 *                    (other IDs, DLC != 8, unknown command, bad padding) and
 *                    INFO are silently ignored.
 * @param data        8 payload bytes.
 * @param reply_data  Out: 8-byte reply payload for the reply ID.
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
