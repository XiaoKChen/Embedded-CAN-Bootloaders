/**
 * @file    can_update.c
 * @brief   CAN Update Protocol v1 state machine (see can_update.h for the
 *          protocol spec and the fail-safe/power-loss table).
 *
 * Implementation notes: every flash mutation goes through the injected HAL;
 * the vector unit and then the record are written only after VERIFY matches,
 * so an interrupted update never leaves a valid record, nor an SP word, over
 * a partial image (boot_decide in image_validate.h then parks the device, or
 * boots the intact old app).
 * Unused-byte rules per command are one bitmask each (bit n = byte n must be
 * 0xFF), indexed by command byte.
 *
 * @see     can_update.h, test_can_update.c
 */

#include "can_update.h"

#include <stddef.h>
#include <string.h>

#include "image_header.h"
#include "image_validate.h"

/* Bit n set: byte n of an accepted frame must be CAN_UPDATE_FILL_BYTE.
 * Indexed by command byte - CAN_UPDATE_CMD_HELLO. */
static const uint8_t k_fill_mask[CAN_UPDATE_CMD_INFO] = {
    0xFCu,  /* HELLO:  bytes 2..7 (byte 1 = version)        */
    0xFEu,  /* ERASE:  bytes 1..7                           */
    0x00u,  /* DATA:   payload, nothing to check            */
    0xC2u,  /* SIZE:   bytes 1, 6, 7 (bytes 2..5 = size)    */
    0xC2u,  /* VERIFY: bytes 1, 6, 7 (bytes 2..5 = CRC32)   */
    0xFEu,  /* GO:     bytes 1..7                           */
    0xFEu,  /* INFO:   bytes 1..7                           */
};

/* -----------------------------------------------------------------------------
 * Private helpers
 * ----------------------------------------------------------------------------- */
static uint32_t load_le32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void store_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

/* Build an ACK (err == CAN_UPDATE_OK) or NAK reply. Returns true so call
 * sites can `return make_reply(...)` directly from handle_frame. */
static bool make_reply(uint8_t reply[8], uint8_t cmd_echo, uint8_t err, uint8_t arg)
{
    reply[0] = CAN_UPDATE_REPLY_MARKER;
    reply[1] = cmd_echo;
    reply[2] = err;
    reply[3] = arg;
    memset(&reply[4], CAN_UPDATE_FILL_BYTE, 4u);
    return true;
}

static uint32_t min_u32(uint32_t a, uint32_t b)
{
    return (a < b) ? a : b;
}

/* Rewind to the start of the current window: drop buffered bytes and expect
 * the window's first seq again. Completed blocks stay counted. */
static void discard_window(can_update_state_t *st)
{
    st->block_fill     = 0u;
    st->bytes_received = st->block_index * FLASH_BLOCK_SIZE;
    st->expected_seq   = st->window_start_seq;
}

/* TRANSFERRED keeps window_start_seq at the last window's first seq and
 * expected_seq one past the final seq: true for a seq of that window. */
static bool in_last_window(const can_update_state_t *st, uint8_t seq)
{
    return (uint8_t)(seq - st->window_start_seq) <=
           (uint8_t)(st->expected_seq - 1u - st->window_start_seq);
}

static void reset_session(can_update_state_t *st)
{
    st->phase            = CAN_UPDATE_PHASE_IDLE;
    st->image_size       = 0u;
    st->bytes_received   = 0u;
    st->block_index      = 0u;
    st->block_fill       = 0u;
    st->expected_seq     = 0u;
    st->window_start_seq = 0u;
}

/* -----------------------------------------------------------------------------
 * Command handlers — each returns true when a reply was produced
 * ----------------------------------------------------------------------------- */
static bool handle_hello(can_update_state_t *st, const uint8_t data[8], uint8_t reply[8])
{
    if (data[1] != CAN_UPDATE_PROTOCOL_VERSION) {
        return make_reply(reply, CAN_UPDATE_CMD_HELLO, CAN_UPDATE_ERR_STATE, 0u);
    }

    /* HELLO (re)starts the session from any phase — host-retry robustness. */
    reset_session(st);
    st->phase = CAN_UPDATE_PHASE_GREETED;

    (void)make_reply(reply, CAN_UPDATE_CMD_HELLO, CAN_UPDATE_OK,
                     CAN_UPDATE_PROTOCOL_VERSION);
    store_le32(&reply[4], APP_SLOT_SIZE);  /* HELLO ACK carries the slot size */
    return true;
}

static bool handle_size(can_update_state_t *st, const uint8_t data[8], uint8_t reply[8])
{
    const uint32_t image_size = load_le32(&data[2]);

    /* SIZE claims flash (can_update_owns_flash): not while USB DFU owns it. */
    if (st->hal->flash_locked()) {
        return make_reply(reply, CAN_UPDATE_CMD_SIZE, CAN_UPDATE_ERR_STATE, 0u);
    }

    if (image_size < IMAGE_MIN_SIZE || image_size > APP_SLOT_SIZE) {
        return make_reply(reply, CAN_UPDATE_CMD_SIZE, CAN_UPDATE_ERR_SIZE, 0u);
    }

    st->image_size = image_size;
    st->phase      = CAN_UPDATE_PHASE_SIZED;
    return make_reply(reply, CAN_UPDATE_CMD_SIZE, CAN_UPDATE_OK, 0u);
}

static bool handle_erase(can_update_state_t *st, uint8_t reply[8])
{
    if (st->hal->flash_locked()) {
        return make_reply(reply, CAN_UPDATE_CMD_ERASE, CAN_UPDATE_ERR_STATE, 0u);
    }
    /* Power loss: until the marker is in place the board does what it did
     * before (old app boots, or parks); from then on it parks. Rows in the
     * can_update.h power-loss table. */
    if (!st->hal->flash_erase_app_and_metadata()) {
        return make_reply(reply, CAN_UPDATE_CMD_ERASE, CAN_UPDATE_ERR_FLASH, 0u);
    }

    st->bytes_received   = 0u;
    st->block_index      = 0u;
    st->block_fill       = 0u;
    st->expected_seq     = 0u;
    st->window_start_seq = 0u;
    st->phase            = CAN_UPDATE_PHASE_TRANSFER;
    return make_reply(reply, CAN_UPDATE_CMD_ERASE, CAN_UPDATE_OK, 0u);
}

static bool handle_data(can_update_state_t *st, const uint8_t data[8], uint8_t reply[8])
{
    const uint8_t seq = data[1];

    if (st->hal->flash_locked()) {
        return make_reply(reply, CAN_UPDATE_CMD_DATA, CAN_UPDATE_ERR_STATE, 0u);
    }

    if (seq != st->expected_seq) {
        const uint8_t rewind_seq = st->window_start_seq;
        discard_window(st);
        return make_reply(reply, CAN_UPDATE_CMD_DATA, CAN_UPDATE_ERR_SEQ, rewind_seq);
    }

    /* Frames never straddle windows (host packs 341x6 + 1x2 per block), but
     * clamp against both the block boundary and image_size regardless. */
    const uint32_t take = min_u32(CAN_UPDATE_DATA_BYTES_PER_FRAME,
                                  min_u32(FLASH_BLOCK_SIZE - st->block_fill,
                                          st->image_size - st->bytes_received));
    memcpy(&st->block_buf[st->block_fill], &data[2], take);
    st->block_fill     += take;
    st->bytes_received += take;
    st->expected_seq++;  /* uint8_t: wraps mod 256 by definition */

    const bool block_full     = (st->block_fill == FLASH_BLOCK_SIZE);
    const bool image_complete = (st->bytes_received == st->image_size);
    if (!block_full && !image_complete) {
        return false;  /* mid-window DATA frames get no reply */
    }

    /* Pad the tail to a whole number of write units with 0xFF (erased value). */
    uint32_t padded_len = st->block_fill;
    const uint32_t remainder = padded_len % FLASH_WRITE_UNIT;
    if (remainder != 0u) {
        const uint32_t pad = FLASH_WRITE_UNIT - remainder;
        memset(&st->block_buf[padded_len], CAN_UPDATE_FILL_BYTE, pad);
        padded_len += pad;
    }

    /* Block 0 skips its vector unit: it waits in RAM until VERIFY commits
     * (image_header.h), so the SP word stays erased over a partial image. */
    uint32_t skip = 0u;
    if (st->block_index == 0u) {
        memcpy(st->vector_unit, st->block_buf, IMAGE_VECTOR_UNIT_SIZE);
        skip = IMAGE_VECTOR_UNIT_SIZE;
    }

    /* Power loss during this program: block torn, no record, marker set ->
     * device stays in the bootloader. A failed program may have written part
     * of the block, which cannot be programmed again without an erase: drop
     * the session so the host restarts from HELLO (and ERASE). */
    if (padded_len > skip &&
        !st->hal->flash_program(APP_BASE + st->block_index * FLASH_BLOCK_SIZE + skip,
                                &st->block_buf[skip], padded_len - skip)) {
        reset_session(st);
        return make_reply(reply, CAN_UPDATE_CMD_DATA, CAN_UPDATE_ERR_FLASH, 0u);
    }

    st->block_index++;
    st->block_fill = 0u;
    if (image_complete) {
        st->phase = CAN_UPDATE_PHASE_TRANSFERRED;  /* window_start_seq stays */
    } else {
        st->window_start_seq = st->expected_seq;
    }
    return make_reply(reply, CAN_UPDATE_CMD_DATA, CAN_UPDATE_OK, seq);
}

static bool handle_verify(can_update_state_t *st, const uint8_t data[8], uint8_t reply[8])
{
    if (st->hal->flash_locked()) {
        return make_reply(reply, CAN_UPDATE_CMD_VERIFY, CAN_UPDATE_ERR_STATE, 0u);
    }

    const uint32_t expected_crc = load_le32(&data[2]);
    const uint8_t *app = st->hal->flash_base + APP_BASE;

    /* Fail closed: nothing is committed over an image that would not
     * validate at boot. The CRC covers the vector unit from RAM and the rest
     * from flash. The session stays TRANSFERRED; the host restarts from
     * HELLO. */
    if (image_check_staged(st->vector_unit, app, st->image_size, expected_crc) !=
        IMAGE_VALID) {
        return make_reply(reply, CAN_UPDATE_CMD_VERIFY, CAN_UPDATE_ERR_CRC, 0u);
    }

    /* Commit, step 1: the vector unit. Power loss during it: no record,
     * marker set -> stay in bootloader. A failed write drops the session;
     * the next ERASE finds the marker and a blank record area and erases the
     * whole slot again (update_start_plan). */
    if (!st->hal->flash_program(APP_BASE, st->vector_unit, IMAGE_VECTOR_UNIT_SIZE)) {
        reset_session(st);
        return make_reply(reply, CAN_UPDATE_CMD_VERIFY, CAN_UPDATE_ERR_FLASH, 0u);
    }

    _Alignas(FLASH_WRITE_UNIT) uint8_t meta[IMAGE_META_SIZE];
    image_meta_encode(meta, st->image_size, expected_crc);

    /* Commit, step 2: the record. Power loss before/during this write:
     * metadata absent or torn (header CRC fails) -> stay in bootloader.
     * After it: the new, already-CRC-verified image boots. 16 bytes = 2
     * write units. A failed write may have left part of the record: drop the
     * session (the next ERASE then erases the APP_BASE and metadata blocks,
     * see update_start_plan). */
    if (!st->hal->flash_program(METADATA_ADDR, meta, IMAGE_META_SIZE)) {
        reset_session(st);
        return make_reply(reply, CAN_UPDATE_CMD_VERIFY, CAN_UPDATE_ERR_FLASH, 0u);
    }

    st->phase = CAN_UPDATE_PHASE_VERIFIED;
    return make_reply(reply, CAN_UPDATE_CMD_VERIFY, CAN_UPDATE_OK, 0u);
}

/* -----------------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------------- */
void can_update_init(can_update_state_t *st, const can_update_hal_t *hal,
                     uint32_t cmd_id)
{
    memset(st, 0, sizeof(*st));
    st->hal    = hal;
    st->cmd_id = cmd_id;  /* per-device host -> boot ID (see dfu_config.h v2) */
    reset_session(st);
}

bool can_update_accepts_frame(const can_update_state_t *st, uint32_t id,
                              uint8_t dlc, const uint8_t data[8])
{
    const uint8_t cmd = data[0];

    if (id != st->cmd_id || dlc != 8u ||
        cmd < CAN_UPDATE_CMD_HELLO || cmd > CAN_UPDATE_CMD_INFO) {
        return false;
    }
    /* DATA carries payload, so it has no padding to check, and its command
     * byte (0x03) is also how a 3-byte ISO-TP single frame starts. Outside a
     * transfer (or the resend of its last window) it can only be such app
     * traffic: ignore it, so it neither draws a NAK nor keeps CAN update
     * mode alive. */
    if (cmd == CAN_UPDATE_CMD_DATA &&
        !(st->phase == CAN_UPDATE_PHASE_TRANSFER ||
          (st->phase == CAN_UPDATE_PHASE_TRANSFERRED && in_last_window(st, data[1])))) {
        return false;
    }
    const uint8_t mask = k_fill_mask[cmd - CAN_UPDATE_CMD_HELLO];
    for (uint8_t i = 1u; i < 8u; i++) {
        if (((mask >> i) & 1u) != 0u && data[i] != CAN_UPDATE_FILL_BYTE) {
            return false;
        }
    }
    return true;
}

bool can_update_owns_flash(const can_update_state_t *st)
{
    return st->phase >= CAN_UPDATE_PHASE_SIZED;
}

bool can_update_handle_frame(can_update_state_t *st, uint32_t id, uint8_t dlc,
                             const uint8_t data[8], uint8_t reply_data[8],
                             can_update_action_t *out_action)
{
    *out_action = CAN_UPDATE_ACTION_NONE;

    /* Not a protocol frame (other ID, app traffic, our own reply marker):
     * answering it would only put noise on the reply ID. */
    if (!can_update_accepts_frame(st, id, dlc, data)) {
        return false;
    }

    const uint8_t cmd = data[0];

    /* HELLO restarts the session from any phase; every other command is
     * valid in exactly one phase, or is an exact repeat of the previous
     * command whose ACK the host did not see (see can_update.h). */
    switch (cmd) {
    case CAN_UPDATE_CMD_HELLO:
        return handle_hello(st, data, reply_data);
    case CAN_UPDATE_CMD_SIZE:
        if (st->phase == CAN_UPDATE_PHASE_GREETED) {
            return handle_size(st, data, reply_data);
        }
        if (st->phase == CAN_UPDATE_PHASE_SIZED &&
            load_le32(&data[2]) == st->image_size) {
            return make_reply(reply_data, cmd, CAN_UPDATE_OK, 0u);
        }
        break;
    case CAN_UPDATE_CMD_ERASE:
        if (st->phase == CAN_UPDATE_PHASE_SIZED) {
            return handle_erase(st, reply_data);
        }
        if (st->phase == CAN_UPDATE_PHASE_TRANSFER && st->bytes_received == 0u) {
            return make_reply(reply_data, cmd, CAN_UPDATE_OK, 0u);
        }
        break;
    case CAN_UPDATE_CMD_DATA:
        if (st->phase == CAN_UPDATE_PHASE_TRANSFER) {
            return handle_data(st, data, reply_data);
        }
        /* TRANSFERRED (accepts_frame admits only the last window's seqs):
         * the host resends that window when its ACK was lost. Nothing is
         * programmed again; its final frame is re-ACKed. */
        if ((uint8_t)(data[1] + 1u) == st->expected_seq) {
            return make_reply(reply_data, cmd, CAN_UPDATE_OK, data[1]);
        }
        return false;
    case CAN_UPDATE_CMD_VERIFY:
        if (st->phase == CAN_UPDATE_PHASE_TRANSFERRED) {
            return handle_verify(st, data, reply_data);
        }
        if (st->phase == CAN_UPDATE_PHASE_VERIFIED &&
            load_le32(&data[2]) == load_le32(st->hal->flash_base + METADATA_ADDR +
                                              offsetof(image_meta_t, image_crc32))) {
            return make_reply(reply_data, cmd, CAN_UPDATE_OK, 0u);
        }
        break;
    case CAN_UPDATE_CMD_GO:
        if (st->phase == CAN_UPDATE_PHASE_VERIFIED) {
            st->phase   = CAN_UPDATE_PHASE_IDLE;
            *out_action = CAN_UPDATE_ACTION_RESET;
            return make_reply(reply_data, CAN_UPDATE_CMD_GO, CAN_UPDATE_OK, 0u);
        }
        break;
    default:
        return false;  /* INFO: answered by the integration layer */
    }

    return make_reply(reply_data, cmd, CAN_UPDATE_ERR_STATE, 0u);
}
