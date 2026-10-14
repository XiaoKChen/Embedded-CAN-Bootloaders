/* =============================================================================
 * test_can_update.c — Host-side Unity tests for the CAN update state machine
 * =============================================================================
 * Fake slot HAL over a static 32 KB array with NOR-like semantics (program
 * only into erased bytes, window-aligned offsets enforced). Covers the happy
 * path end-to-end (transfer -> read-back CRC -> commit -> activate on GO),
 * every NAK path, window rewind after a seq gap, flash/read/commit/activate
 * failure injection, and the final short window.
 * ============================================================================= */

#include <string.h>

#include "unity.h"

#include "can_update.h"
#include "crc32.h"

/* -----------------------------------------------------------------------------
 * Fake slot + HAL
 * ----------------------------------------------------------------------------- */
#define TEST_SLOT_SIZE (16u * CAN_UPDATE_WINDOW_SIZE)  /* 32 KB */

static uint8_t  s_slot[TEST_SLOT_SIZE];
static bool     s_session_open;
static bool     s_fail_erase;
static bool     s_fail_next_program;
static bool     s_fail_read;
static bool     s_fail_commit;
static bool     s_fail_next_activate;
static uint32_t s_commit_calls;
static uint32_t s_activate_calls;
static uint32_t s_last_program_offset;
static uint32_t s_last_program_len;

static bool fake_slot_erase(void)
{
    if (s_fail_erase) {
        return false;
    }
    memset(s_slot, 0xFF, sizeof(s_slot));
    s_session_open = true;
    return true;
}

static bool fake_slot_program(uint32_t offset, const uint8_t *data, uint32_t len)
{
    if (s_fail_next_program) {
        s_fail_next_program = false;  /* simulated read-back-verify failure */
        return false;
    }

    /* Enforce the contract the engine promised the HAL. */
    TEST_ASSERT_TRUE(s_session_open);
    TEST_ASSERT_EQUAL_UINT32(0u, offset % CAN_UPDATE_WINDOW_SIZE);
    TEST_ASSERT_TRUE(len > 0u && len <= CAN_UPDATE_WINDOW_SIZE);
    TEST_ASSERT_TRUE((uint64_t)offset + len <= TEST_SLOT_SIZE);
    for (uint32_t i = 0u; i < len; i++) {
        TEST_ASSERT_EQUAL_HEX8(0xFFu, s_slot[offset + i]);  /* NOR: erased only */
    }

    memcpy(&s_slot[offset], data, len);
    s_last_program_offset = offset;
    s_last_program_len    = len;
    return true;
}

static bool fake_slot_read(uint32_t offset, uint8_t *buf, uint32_t len)
{
    if (s_fail_read) {
        return false;
    }
    TEST_ASSERT_TRUE((uint64_t)offset + len <= TEST_SLOT_SIZE);
    memcpy(buf, &s_slot[offset], len);
    return true;
}

static bool fake_slot_commit(void)
{
    s_commit_calls++;
    return !s_fail_commit;
}

static bool fake_slot_activate(void)
{
    s_activate_calls++;
    if (s_fail_next_activate) {
        s_fail_next_activate = false;
        return false;
    }
    return true;
}

static const can_update_hal_t s_hal = {
    .slot_size     = TEST_SLOT_SIZE,
    .slot_erase    = fake_slot_erase,
    .slot_program  = fake_slot_program,
    .slot_read     = fake_slot_read,
    .slot_commit   = fake_slot_commit,
    .slot_activate = fake_slot_activate,
};

/* -----------------------------------------------------------------------------
 * Frame plumbing
 * ----------------------------------------------------------------------------- */
static can_update_state_t  s_state;
static uint8_t             s_reply[8];
static bool                s_replied;
static can_update_action_t s_action;
static uint32_t            s_cmd_id;  /* host -> boot ID the engine is keyed on */

static void store_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static bool send_frame(const uint8_t data[8])
{
    memset(s_reply, 0, sizeof(s_reply));
    s_replied = can_update_handle_frame(&s_state, s_cmd_id, 8u, data,
                                        s_reply, &s_action);
    return s_replied;
}

/* Command frame with a LE word in bytes 2..5 (SIZE/VERIFY) or all-0xFF fill. */
static bool send_cmd(uint8_t cmd, uint8_t byte1, bool has_word, uint32_t word)
{
    uint8_t d[8];
    memset(d, 0xFF, sizeof(d));
    d[0] = cmd;
    d[1] = byte1;
    if (has_word) {
        store_le32(&d[2], word);
    }
    return send_frame(d);
}

static void assert_reply(uint8_t cmd_echo, uint8_t err, uint8_t arg)
{
    TEST_ASSERT_TRUE(s_replied);
    TEST_ASSERT_EQUAL_HEX8(CAN_UPDATE_REPLY_MARKER, s_reply[0]);
    TEST_ASSERT_EQUAL_HEX8(cmd_echo, s_reply[1]);
    TEST_ASSERT_EQUAL_HEX8(err, s_reply[2]);
    TEST_ASSERT_EQUAL_HEX8(arg, s_reply[3]);
}

/* -----------------------------------------------------------------------------
 * Synthetic image: ~5 KB, deliberately awkward sizes
 *   5003 = 2 full windows + a 907-byte final window; 907 = 151x6 + 1 (short
 *   final DATA frame). VERIFY reads it back in 3 chunks.
 * ----------------------------------------------------------------------------- */
#define TEST_IMAGE_SIZE       5003u
#define TEST_FINAL_WINDOW_LEN (TEST_IMAGE_SIZE - 2u * CAN_UPDATE_WINDOW_SIZE)  /* 907 */

static uint8_t s_image[TEST_IMAGE_SIZE];
static uint8_t s_seq;  /* running DATA seq the "host" side maintains */

static void build_test_image(void)
{
    for (uint32_t i = 0u; i < TEST_IMAGE_SIZE; i++) {
        s_image[i] = (uint8_t)(i * 31u + 7u);
    }
}

/* Stream one window of DATA frames. Mid-window frames must produce no reply;
 * the window-completing frame must be ACKed with arg = its seq. */
static void stream_block(uint32_t block_start)
{
    const uint32_t block_len = (TEST_IMAGE_SIZE - block_start < CAN_UPDATE_WINDOW_SIZE)
                                   ? (TEST_IMAGE_SIZE - block_start)
                                   : CAN_UPDATE_WINDOW_SIZE;
    uint32_t off = 0u;
    while (off < block_len) {
        uint32_t chunk = block_len - off;
        if (chunk > CAN_UPDATE_DATA_BYTES_PER_FRAME) {
            chunk = CAN_UPDATE_DATA_BYTES_PER_FRAME;
        }

        uint8_t d[8];
        memset(d, 0xFF, sizeof(d));
        d[0] = CAN_UPDATE_CMD_DATA;
        d[1] = s_seq;
        memcpy(&d[2], &s_image[block_start + off], chunk);

        const bool is_block_final = (off + chunk == block_len);
        const bool replied        = send_frame(d);
        if (is_block_final) {
            assert_reply(CAN_UPDATE_CMD_DATA, CAN_UPDATE_OK, s_seq);
        } else {
            TEST_ASSERT_FALSE(replied);
        }
        s_seq++;
        off += chunk;
    }
}

/* Drive HELLO -> SIZE -> ERASE and leave the engine awaiting DATA. */
static void enter_transfer_phase(void)
{
    TEST_ASSERT_TRUE(send_cmd(CAN_UPDATE_CMD_HELLO,
                              CAN_UPDATE_PROTOCOL_VERSION, false, 0u));
    assert_reply(CAN_UPDATE_CMD_HELLO, CAN_UPDATE_OK, CAN_UPDATE_PROTOCOL_VERSION);

    TEST_ASSERT_TRUE(send_cmd(CAN_UPDATE_CMD_SIZE, 0xFFu, true, TEST_IMAGE_SIZE));
    assert_reply(CAN_UPDATE_CMD_SIZE, CAN_UPDATE_OK, 0u);

    TEST_ASSERT_TRUE(send_cmd(CAN_UPDATE_CMD_ERASE, 0xFFu, false, 0u));
    assert_reply(CAN_UPDATE_CMD_ERASE, CAN_UPDATE_OK, 0u);

    s_seq = 0u;
}

static void transfer_whole_image(void)
{
    enter_transfer_phase();
    for (uint32_t start = 0u; start < TEST_IMAGE_SIZE; start += CAN_UPDATE_WINDOW_SIZE) {
        stream_block(start);
    }
}

static void verify_good_crc(void)
{
    const uint32_t image_crc = crc32_ieee(s_image, TEST_IMAGE_SIZE);
    TEST_ASSERT_TRUE(send_cmd(CAN_UPDATE_CMD_VERIFY, 0xFFu, true, image_crc));
}

void setUp(void)
{
    /* Fresh non-erased slot: any program before ERASE would trip the fake's
     * NOR check, proving the engine never writes an unerased window. */
    memset(s_slot, 0x00, sizeof(s_slot));
    s_session_open        = false;
    s_fail_erase          = false;
    s_fail_next_program   = false;
    s_fail_read           = false;
    s_fail_commit         = false;
    s_fail_next_activate  = false;
    s_commit_calls        = 0u;
    s_activate_calls      = 0u;
    s_last_program_offset = 0u;
    s_last_program_len    = 0u;
    s_seq                 = 0u;
    s_action              = CAN_UPDATE_ACTION_NONE;
    s_cmd_id              = CAN_ID_HOST_TO_BOOT;  /* default; overridden per-test */
    build_test_image();
    can_update_init(&s_state, &s_hal, s_cmd_id);
}

void tearDown(void) {}

/* -----------------------------------------------------------------------------
 * Tests
 * ----------------------------------------------------------------------------- */
static void test_frames_on_other_ids_are_ignored(void)
{
    uint8_t d[8];
    memset(d, 0xFF, sizeof(d));
    d[0] = CAN_UPDATE_CMD_HELLO;
    d[1] = CAN_UPDATE_PROTOCOL_VERSION;
    TEST_ASSERT_FALSE(can_update_handle_frame(&s_state, 0x123u, 8u, d, s_reply,
                                              &s_action));
    TEST_ASSERT_FALSE(can_update_handle_frame(&s_state, CAN_ID_BOOT_TO_HOST, 8u,
                                              d, s_reply, &s_action));
}

static void test_hello_ack_carries_version_and_hal_slot_size(void)
{
    TEST_ASSERT_TRUE(send_cmd(CAN_UPDATE_CMD_HELLO,
                              CAN_UPDATE_PROTOCOL_VERSION, false, 0u));
    assert_reply(CAN_UPDATE_CMD_HELLO, CAN_UPDATE_OK, CAN_UPDATE_PROTOCOL_VERSION);

    uint8_t expected_size[4];
    store_le32(expected_size, TEST_SLOT_SIZE);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected_size, &s_reply[4], 4);
}

static void test_hello_wrong_version_naks(void)
{
    TEST_ASSERT_TRUE(send_cmd(CAN_UPDATE_CMD_HELLO, 0x02u, false, 0u));
    assert_reply(CAN_UPDATE_CMD_HELLO, CAN_UPDATE_ERR_STATE, 0u);
}

static void test_happy_path_full_update(void)
{
    transfer_whole_image();

    /* Final short window: exactly its 907 image bytes, no padding — write-unit
     * alignment is the HAL's business. Bytes past the image stay erased. */
    TEST_ASSERT_EQUAL_UINT32(2u * CAN_UPDATE_WINDOW_SIZE, s_last_program_offset);
    TEST_ASSERT_EQUAL_UINT32(TEST_FINAL_WINDOW_LEN, s_last_program_len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(s_image, s_slot, TEST_IMAGE_SIZE);
    for (uint32_t i = TEST_IMAGE_SIZE; i < 3u * CAN_UPDATE_WINDOW_SIZE; i++) {
        TEST_ASSERT_EQUAL_HEX8(0xFFu, s_slot[i]);
    }

    /* VERIFY CRCs the slot read-back, then hands it to the target. */
    verify_good_crc();
    assert_reply(CAN_UPDATE_CMD_VERIFY, CAN_UPDATE_OK, 0u);
    TEST_ASSERT_EQUAL_UINT32(1u, s_commit_calls);
    TEST_ASSERT_EQUAL_UINT32(0u, s_activate_calls);  /* not bootable before GO */

    /* GO activates the slot, then requests the reset. */
    TEST_ASSERT_TRUE(send_cmd(CAN_UPDATE_CMD_GO, 0xFFu, false, 0u));
    assert_reply(CAN_UPDATE_CMD_GO, CAN_UPDATE_OK, 0u);
    TEST_ASSERT_EQUAL_UINT32(1u, s_activate_calls);
    TEST_ASSERT_EQUAL(CAN_UPDATE_ACTION_RESET, s_action);
}

static void test_out_of_order_command_naks_state(void)
{
    /* SIZE before HELLO. */
    TEST_ASSERT_TRUE(send_cmd(CAN_UPDATE_CMD_SIZE, 0xFFu, true, TEST_IMAGE_SIZE));
    assert_reply(CAN_UPDATE_CMD_SIZE, CAN_UPDATE_ERR_STATE, 0u);

    /* ERASE straight after HELLO (SIZE skipped). */
    TEST_ASSERT_TRUE(send_cmd(CAN_UPDATE_CMD_HELLO,
                              CAN_UPDATE_PROTOCOL_VERSION, false, 0u));
    TEST_ASSERT_TRUE(send_cmd(CAN_UPDATE_CMD_ERASE, 0xFFu, false, 0u));
    assert_reply(CAN_UPDATE_CMD_ERASE, CAN_UPDATE_ERR_STATE, 0u);

    /* Unknown command. */
    TEST_ASSERT_TRUE(send_cmd(0x7Fu, 0xFFu, false, 0u));
    assert_reply(0x7Fu, CAN_UPDATE_ERR_STATE, 0u);
}

static void test_go_before_verify_naks_state(void)
{
    transfer_whole_image();  /* image fully programmed, VERIFY not sent */
    TEST_ASSERT_TRUE(send_cmd(CAN_UPDATE_CMD_GO, 0xFFu, false, 0u));
    assert_reply(CAN_UPDATE_CMD_GO, CAN_UPDATE_ERR_STATE, 0u);
    TEST_ASSERT_EQUAL(CAN_UPDATE_ACTION_NONE, s_action);
    TEST_ASSERT_EQUAL_UINT32(0u, s_activate_calls);
}

static void test_size_out_of_bounds_naks(void)
{
    TEST_ASSERT_TRUE(send_cmd(CAN_UPDATE_CMD_HELLO,
                              CAN_UPDATE_PROTOCOL_VERSION, false, 0u));

    TEST_ASSERT_TRUE(send_cmd(CAN_UPDATE_CMD_SIZE, 0xFFu, true, 0u));
    assert_reply(CAN_UPDATE_CMD_SIZE, CAN_UPDATE_ERR_SIZE, 0u);

    TEST_ASSERT_TRUE(send_cmd(CAN_UPDATE_CMD_SIZE, 0xFFu, true,
                              TEST_SLOT_SIZE + 1u));
    assert_reply(CAN_UPDATE_CMD_SIZE, CAN_UPDATE_ERR_SIZE, 0u);

    /* Still awaiting SIZE: a legal size is accepted after the NAKs. */
    TEST_ASSERT_TRUE(send_cmd(CAN_UPDATE_CMD_SIZE, 0xFFu, true, TEST_SLOT_SIZE));
    assert_reply(CAN_UPDATE_CMD_SIZE, CAN_UPDATE_OK, 0u);
}

static void test_seq_gap_mid_window_rewinds_and_recovers(void)
{
    enter_transfer_phase();
    stream_block(0u);  /* window 0 done; window 1 starts at seq 86 */
    const uint8_t window1_start = s_seq;
    TEST_ASSERT_EQUAL_HEX8(86u, window1_start);

    /* A few good frames into window 1, then skip one seq. */
    for (uint32_t i = 0u; i < 5u; i++) {
        uint8_t d[8];
        memset(d, 0xFF, sizeof(d));
        d[0] = CAN_UPDATE_CMD_DATA;
        d[1] = s_seq;
        memcpy(&d[2], &s_image[CAN_UPDATE_WINDOW_SIZE + i * 6u], 6u);
        TEST_ASSERT_FALSE(send_frame(d));
        s_seq++;
    }
    uint8_t gap[8];
    memset(gap, 0xFF, sizeof(gap));
    gap[0] = CAN_UPDATE_CMD_DATA;
    gap[1] = (uint8_t)(s_seq + 1u);  /* one frame lost */
    TEST_ASSERT_TRUE(send_frame(gap));
    assert_reply(CAN_UPDATE_CMD_DATA, CAN_UPDATE_ERR_SEQ, window1_start);

    /* Retransmit window 1 from its first seq, then finish the image. */
    s_seq = window1_start;
    stream_block(CAN_UPDATE_WINDOW_SIZE);
    stream_block(2u * CAN_UPDATE_WINDOW_SIZE);

    verify_good_crc();
    assert_reply(CAN_UPDATE_CMD_VERIFY, CAN_UPDATE_OK, 0u);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(s_image, s_slot, TEST_IMAGE_SIZE);
}

static void test_verify_wrong_crc_naks_and_skips_commit(void)
{
    transfer_whole_image();

    const uint32_t wrong_crc = crc32_ieee(s_image, TEST_IMAGE_SIZE) ^ 0xDEADBEEFu;
    TEST_ASSERT_TRUE(send_cmd(CAN_UPDATE_CMD_VERIFY, 0xFFu, true, wrong_crc));
    assert_reply(CAN_UPDATE_CMD_VERIFY, CAN_UPDATE_ERR_CRC, 0u);

    /* Fail closed: the target never sees the slot, GO stays refused. */
    TEST_ASSERT_EQUAL_UINT32(0u, s_commit_calls);
    TEST_ASSERT_TRUE(send_cmd(CAN_UPDATE_CMD_GO, 0xFFu, false, 0u));
    assert_reply(CAN_UPDATE_CMD_GO, CAN_UPDATE_ERR_STATE, 0u);
    TEST_ASSERT_EQUAL_UINT32(0u, s_activate_calls);
}

static void test_verify_read_failure_naks_flash_error(void)
{
    transfer_whole_image();

    s_fail_read = true;
    verify_good_crc();
    assert_reply(CAN_UPDATE_CMD_VERIFY, CAN_UPDATE_ERR_FLASH, 0u);
    TEST_ASSERT_EQUAL_UINT32(0u, s_commit_calls);

    /* Still awaiting VERIFY: it succeeds once reads work again. */
    s_fail_read = false;
    verify_good_crc();
    assert_reply(CAN_UPDATE_CMD_VERIFY, CAN_UPDATE_OK, 0u);
}

static void test_verify_target_rejects_image_naks_image_error(void)
{
    transfer_whole_image();

    s_fail_commit = true;  /* e.g. esp_ota_end: wrong chip / bad SHA-256 */
    verify_good_crc();
    assert_reply(CAN_UPDATE_CMD_VERIFY, CAN_UPDATE_ERR_IMAGE, 0u);
    TEST_ASSERT_EQUAL_UINT32(1u, s_commit_calls);

    TEST_ASSERT_TRUE(send_cmd(CAN_UPDATE_CMD_GO, 0xFFu, false, 0u));
    assert_reply(CAN_UPDATE_CMD_GO, CAN_UPDATE_ERR_STATE, 0u);
    TEST_ASSERT_EQUAL_UINT32(0u, s_activate_calls);
}

static void test_go_activate_failure_naks_and_retry_succeeds(void)
{
    transfer_whole_image();
    verify_good_crc();
    assert_reply(CAN_UPDATE_CMD_VERIFY, CAN_UPDATE_OK, 0u);

    s_fail_next_activate = true;
    TEST_ASSERT_TRUE(send_cmd(CAN_UPDATE_CMD_GO, 0xFFu, false, 0u));
    assert_reply(CAN_UPDATE_CMD_GO, CAN_UPDATE_ERR_FLASH, 0u);
    TEST_ASSERT_EQUAL(CAN_UPDATE_ACTION_NONE, s_action);  /* no reset */

    /* Phase stays VERIFIED, so the host's GO retry goes through. */
    TEST_ASSERT_TRUE(send_cmd(CAN_UPDATE_CMD_GO, 0xFFu, false, 0u));
    assert_reply(CAN_UPDATE_CMD_GO, CAN_UPDATE_OK, 0u);
    TEST_ASSERT_EQUAL(CAN_UPDATE_ACTION_RESET, s_action);
    TEST_ASSERT_EQUAL_UINT32(2u, s_activate_calls);
}

static void test_erase_failure_naks_flash_error(void)
{
    TEST_ASSERT_TRUE(send_cmd(CAN_UPDATE_CMD_HELLO,
                              CAN_UPDATE_PROTOCOL_VERSION, false, 0u));
    TEST_ASSERT_TRUE(send_cmd(CAN_UPDATE_CMD_SIZE, 0xFFu, true, TEST_IMAGE_SIZE));

    s_fail_erase = true;
    TEST_ASSERT_TRUE(send_cmd(CAN_UPDATE_CMD_ERASE, 0xFFu, false, 0u));
    assert_reply(CAN_UPDATE_CMD_ERASE, CAN_UPDATE_ERR_FLASH, 0u);

    /* ERASE may be retried once the fault clears. */
    s_fail_erase = false;
    TEST_ASSERT_TRUE(send_cmd(CAN_UPDATE_CMD_ERASE, 0xFFu, false, 0u));
    assert_reply(CAN_UPDATE_CMD_ERASE, CAN_UPDATE_OK, 0u);
}

static void test_program_failure_naks_and_window_retransmit_recovers(void)
{
    enter_transfer_phase();

    /* Stream window 0 with the program call rigged to fail: mid-window frames
     * stay silent, the window-completing frame NAKs err 0x02. */
    s_fail_next_program = true;
    uint32_t off = 0u;
    while (off < CAN_UPDATE_WINDOW_SIZE) {
        const uint32_t chunk = (CAN_UPDATE_WINDOW_SIZE - off > 6u)
                                   ? 6u : (CAN_UPDATE_WINDOW_SIZE - off);
        uint8_t d[8];
        memset(d, 0xFF, sizeof(d));
        d[0] = CAN_UPDATE_CMD_DATA;
        d[1] = s_seq;
        memcpy(&d[2], &s_image[off], chunk);
        const bool replied = send_frame(d);
        if (off + chunk == CAN_UPDATE_WINDOW_SIZE) {
            assert_reply(CAN_UPDATE_CMD_DATA, CAN_UPDATE_ERR_FLASH, 0u);
        } else {
            TEST_ASSERT_FALSE(replied);
        }
        s_seq++;
        off += chunk;
    }

    /* Window discarded: retransmit window 0 from seq 0 and finish the update. */
    s_seq = 0u;
    for (uint32_t start = 0u; start < TEST_IMAGE_SIZE; start += CAN_UPDATE_WINDOW_SIZE) {
        stream_block(start);
    }
    verify_good_crc();
    assert_reply(CAN_UPDATE_CMD_VERIFY, CAN_UPDATE_OK, 0u);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(s_image, s_slot, TEST_IMAGE_SIZE);
}

static void test_hello_restarts_session_mid_transfer(void)
{
    enter_transfer_phase();
    stream_block(0u);  /* one window in */

    /* HELLO resets the session; the next required command is SIZE again. */
    TEST_ASSERT_TRUE(send_cmd(CAN_UPDATE_CMD_HELLO,
                              CAN_UPDATE_PROTOCOL_VERSION, false, 0u));
    assert_reply(CAN_UPDATE_CMD_HELLO, CAN_UPDATE_OK, CAN_UPDATE_PROTOCOL_VERSION);

    TEST_ASSERT_TRUE(send_cmd(CAN_UPDATE_CMD_DATA, 0u, false, 0u));
    assert_reply(CAN_UPDATE_CMD_DATA, CAN_UPDATE_ERR_STATE, 0u);

    TEST_ASSERT_TRUE(send_cmd(CAN_UPDATE_CMD_SIZE, 0xFFu, true, TEST_IMAGE_SIZE));
    assert_reply(CAN_UPDATE_CMD_SIZE, CAN_UPDATE_OK, 0u);
}

static void test_malformed_dlc_naks_state(void)
{
    uint8_t d[8];
    memset(d, 0xFF, sizeof(d));
    d[0] = CAN_UPDATE_CMD_HELLO;
    d[1] = CAN_UPDATE_PROTOCOL_VERSION;
    TEST_ASSERT_TRUE(can_update_handle_frame(&s_state, CAN_ID_HOST_TO_BOOT, 4u,
                                             d, s_reply, &s_action));
    assert_reply(CAN_UPDATE_CMD_HELLO, CAN_UPDATE_ERR_STATE, 0u);
}

/* Per-device ID: an engine keyed on a non-default cmd_id answers only on that
 * ID (the default 0x79E is now ignored), and the full protocol still works. */
static void test_custom_cmd_id_is_matched_and_default_ignored(void)
{
    const uint32_t custom_id = 0x123u;
    s_cmd_id = custom_id;
    can_update_init(&s_state, &s_hal, custom_id);

    /* The old default ID no longer addresses this engine. */
    uint8_t hello[8];
    memset(hello, 0xFF, sizeof(hello));
    hello[0] = CAN_UPDATE_CMD_HELLO;
    hello[1] = CAN_UPDATE_PROTOCOL_VERSION;
    TEST_ASSERT_FALSE(can_update_handle_frame(&s_state, CAN_ID_HOST_TO_BOOT, 8u,
                                              hello, s_reply, &s_action));

    /* Full transfer on the custom ID: framing/acks unchanged. */
    transfer_whole_image();
    verify_good_crc();
    assert_reply(CAN_UPDATE_CMD_VERIFY, CAN_UPDATE_OK, 0u);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(s_image, s_slot, TEST_IMAGE_SIZE);

    TEST_ASSERT_TRUE(send_cmd(CAN_UPDATE_CMD_GO, 0xFFu, false, 0u));
    assert_reply(CAN_UPDATE_CMD_GO, CAN_UPDATE_OK, 0u);
    TEST_ASSERT_EQUAL(CAN_UPDATE_ACTION_RESET, s_action);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_frames_on_other_ids_are_ignored);
    RUN_TEST(test_hello_ack_carries_version_and_hal_slot_size);
    RUN_TEST(test_hello_wrong_version_naks);
    RUN_TEST(test_happy_path_full_update);
    RUN_TEST(test_out_of_order_command_naks_state);
    RUN_TEST(test_go_before_verify_naks_state);
    RUN_TEST(test_size_out_of_bounds_naks);
    RUN_TEST(test_seq_gap_mid_window_rewinds_and_recovers);
    RUN_TEST(test_verify_wrong_crc_naks_and_skips_commit);
    RUN_TEST(test_verify_read_failure_naks_flash_error);
    RUN_TEST(test_verify_target_rejects_image_naks_image_error);
    RUN_TEST(test_go_activate_failure_naks_and_retry_succeeds);
    RUN_TEST(test_erase_failure_naks_flash_error);
    RUN_TEST(test_program_failure_naks_and_window_retransmit_recovers);
    RUN_TEST(test_hello_restarts_session_mid_transfer);
    RUN_TEST(test_malformed_dlc_naks_state);
    RUN_TEST(test_custom_cmd_id_is_matched_and_default_ignored);
    return UNITY_END();
}
