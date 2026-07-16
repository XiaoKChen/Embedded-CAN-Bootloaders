/* =============================================================================
 * test_can_update.c — Host-side Unity tests for the CAN update state machine
 * =============================================================================
 * Fake HAL over a static 256 KB flash array with NOR-like semantics (program
 * only into erased bytes, write-unit alignment enforced). Covers the happy
 * path end-to-end (transfer -> metadata -> image_validate == IMAGE_VALID),
 * every NAK path, window rewind after a seq gap, flash failure injection,
 * and final-short-block 0xFF padding.
 * ============================================================================= */

#include <string.h>

#include "unity.h"

#include "can_update.h"
#include "crc32.h"
#include "image_validate.h"

/* -----------------------------------------------------------------------------
 * Fake flash + HAL
 * ----------------------------------------------------------------------------- */
static uint8_t  s_flash[FLASH_SIZE];
static bool     s_fail_erase;
static bool     s_fail_next_program;
static uint32_t s_last_program_addr;
static uint32_t s_last_program_len;

static bool fake_erase_app_and_metadata(void)
{
    if (s_fail_erase) {
        return false;
    }
    /* Contract: metadata block first, then the app slot. The fake is atomic,
     * so the ordering only matters on real hardware. */
    memset(&s_flash[METADATA_ADDR], 0xFF, METADATA_BLOCK_SIZE);
    memset(&s_flash[APP_BASE], 0xFF, APP_SLOT_SIZE);
    return true;
}

static bool fake_program(uint32_t addr, const uint8_t *data, uint32_t len)
{
    if (s_fail_next_program) {
        s_fail_next_program = false;  /* simulated read-back-verify failure */
        return false;
    }

    /* Enforce the flash discipline the engine promised. */
    TEST_ASSERT_EQUAL_UINT32(0u, addr % FLASH_WRITE_UNIT);
    TEST_ASSERT_EQUAL_UINT32(0u, len % FLASH_WRITE_UNIT);
    TEST_ASSERT_TRUE(len > 0u);
    TEST_ASSERT_TRUE((uint64_t)addr + len <= FLASH_SIZE);
    for (uint32_t i = 0u; i < len; i++) {
        TEST_ASSERT_EQUAL_HEX8(0xFFu, s_flash[addr + i]);  /* NOR: erased only */
    }

    memcpy(&s_flash[addr], data, len);
    s_last_program_addr = addr;
    s_last_program_len  = len;
    return true;
}

static const can_update_hal_t s_hal = {
    .flash_erase_app_and_metadata = fake_erase_app_and_metadata,
    .flash_program                = fake_program,
    .flash_base                   = s_flash,
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
 *   5003 = 2 full blocks + 907-byte final block; 907 = 151x6 + 1 (short final
 *   DATA frame), pads to 912 (multiple of FLASH_WRITE_UNIT).
 * ----------------------------------------------------------------------------- */
#define TEST_IMAGE_SIZE       5003u
#define TEST_FINAL_BLOCK_LEN  (TEST_IMAGE_SIZE - 2u * FLASH_BLOCK_SIZE)  /* 907 */
#define TEST_FINAL_PADDED_LEN 912u

static uint8_t s_image[TEST_IMAGE_SIZE];
static uint8_t s_seq;  /* running DATA seq the "host" side maintains */

static void build_test_image(void)
{
    for (uint32_t i = 0u; i < TEST_IMAGE_SIZE; i++) {
        s_image[i] = (uint8_t)(i * 31u + 7u);
    }
    /* Real vectors so the flashed image also passes image_validate(). */
    store_le32(&s_image[0], SRAM_END);                /* initial SP  */
    store_le32(&s_image[4], (APP_BASE + 0x200u) | 1u); /* Thumb entry */
}

/* Stream one window (block) of DATA frames. Mid-window frames must produce
 * no reply; the block-completing frame must be ACKed with arg = its seq. */
static void stream_block(uint32_t block_start)
{
    const uint32_t block_len = (TEST_IMAGE_SIZE - block_start < FLASH_BLOCK_SIZE)
                                   ? (TEST_IMAGE_SIZE - block_start)
                                   : FLASH_BLOCK_SIZE;
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
    for (uint32_t start = 0u; start < TEST_IMAGE_SIZE; start += FLASH_BLOCK_SIZE) {
        stream_block(start);
    }
}

void setUp(void)
{
    /* Fresh non-erased flash: any program before ERASE would trip the fake's
     * NOR check, proving the engine never writes an unerased block. */
    memset(s_flash, 0x00, sizeof(s_flash));
    s_fail_erase         = false;
    s_fail_next_program  = false;
    s_last_program_addr  = 0u;
    s_last_program_len   = 0u;
    s_seq                = 0u;
    s_action             = CAN_UPDATE_ACTION_NONE;
    s_cmd_id             = CAN_ID_HOST_TO_BOOT;  /* default; overridden per-test */
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

static void test_hello_ack_carries_version_and_slot_size(void)
{
    TEST_ASSERT_TRUE(send_cmd(CAN_UPDATE_CMD_HELLO,
                              CAN_UPDATE_PROTOCOL_VERSION, false, 0u));
    assert_reply(CAN_UPDATE_CMD_HELLO, CAN_UPDATE_OK, CAN_UPDATE_PROTOCOL_VERSION);

    uint8_t expected_size[4];
    store_le32(expected_size, APP_SLOT_SIZE);
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

    /* Final short block: 907 bytes padded with 0xFF to 912 (multiple of 8). */
    TEST_ASSERT_EQUAL_UINT32(APP_BASE + 2u * FLASH_BLOCK_SIZE, s_last_program_addr);
    TEST_ASSERT_EQUAL_UINT32(TEST_FINAL_PADDED_LEN, s_last_program_len);
    for (uint32_t i = TEST_IMAGE_SIZE; i < 2u * FLASH_BLOCK_SIZE + TEST_FINAL_PADDED_LEN; i++) {
        TEST_ASSERT_EQUAL_HEX8(0xFFu, s_flash[APP_BASE + i]);
    }
    TEST_ASSERT_EQUAL_HEX8_ARRAY(s_image, &s_flash[APP_BASE], TEST_IMAGE_SIZE);

    /* VERIFY commits the metadata block. */
    const uint32_t image_crc = crc32_ieee(s_image, TEST_IMAGE_SIZE);
    TEST_ASSERT_TRUE(send_cmd(CAN_UPDATE_CMD_VERIFY, 0xFFu, true, image_crc));
    assert_reply(CAN_UPDATE_CMD_VERIFY, CAN_UPDATE_OK, 0u);

    uint8_t expected_meta[IMAGE_META_SIZE];
    store_le32(&expected_meta[0], IMAGE_META_MAGIC);
    store_le32(&expected_meta[4], TEST_IMAGE_SIZE);
    store_le32(&expected_meta[8], image_crc);
    store_le32(&expected_meta[12],
               crc32_ieee(expected_meta, IMAGE_META_CRC_COVERAGE));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected_meta, &s_flash[METADATA_ADDR],
                                 IMAGE_META_SIZE);

    /* The flashed result passes the boot-time validator. */
    TEST_ASSERT_EQUAL(IMAGE_VALID, image_validate(s_flash));

    /* GO is only now legal and requests the reset. */
    TEST_ASSERT_TRUE(send_cmd(CAN_UPDATE_CMD_GO, 0xFFu, false, 0u));
    assert_reply(CAN_UPDATE_CMD_GO, CAN_UPDATE_OK, 0u);
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
}

static void test_size_out_of_bounds_naks(void)
{
    TEST_ASSERT_TRUE(send_cmd(CAN_UPDATE_CMD_HELLO,
                              CAN_UPDATE_PROTOCOL_VERSION, false, 0u));

    TEST_ASSERT_TRUE(send_cmd(CAN_UPDATE_CMD_SIZE, 0xFFu, true, 0u));
    assert_reply(CAN_UPDATE_CMD_SIZE, CAN_UPDATE_ERR_SIZE, 0u);

    TEST_ASSERT_TRUE(send_cmd(CAN_UPDATE_CMD_SIZE, 0xFFu, true,
                              APP_SLOT_SIZE + 1u));
    assert_reply(CAN_UPDATE_CMD_SIZE, CAN_UPDATE_ERR_SIZE, 0u);

    /* Still awaiting SIZE: a legal size is accepted after the NAKs. */
    TEST_ASSERT_TRUE(send_cmd(CAN_UPDATE_CMD_SIZE, 0xFFu, true, APP_SLOT_SIZE));
    assert_reply(CAN_UPDATE_CMD_SIZE, CAN_UPDATE_OK, 0u);
}

static void test_seq_gap_mid_window_rewinds_and_recovers(void)
{
    enter_transfer_phase();
    stream_block(0u);  /* block 0 done; window 1 starts at seq 86 */
    const uint8_t window1_start = s_seq;
    TEST_ASSERT_EQUAL_HEX8(86u, window1_start);

    /* A few good frames into window 1, then skip one seq. */
    for (uint32_t i = 0u; i < 5u; i++) {
        uint8_t d[8];
        memset(d, 0xFF, sizeof(d));
        d[0] = CAN_UPDATE_CMD_DATA;
        d[1] = s_seq;
        memcpy(&d[2], &s_image[FLASH_BLOCK_SIZE + i * 6u], 6u);
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
    stream_block(FLASH_BLOCK_SIZE);
    stream_block(2u * FLASH_BLOCK_SIZE);

    const uint32_t image_crc = crc32_ieee(s_image, TEST_IMAGE_SIZE);
    TEST_ASSERT_TRUE(send_cmd(CAN_UPDATE_CMD_VERIFY, 0xFFu, true, image_crc));
    assert_reply(CAN_UPDATE_CMD_VERIFY, CAN_UPDATE_OK, 0u);
    TEST_ASSERT_EQUAL(IMAGE_VALID, image_validate(s_flash));
}

static void test_verify_wrong_crc_naks_and_leaves_metadata_erased(void)
{
    transfer_whole_image();

    const uint32_t wrong_crc = crc32_ieee(s_image, TEST_IMAGE_SIZE) ^ 0xDEADBEEFu;
    TEST_ASSERT_TRUE(send_cmd(CAN_UPDATE_CMD_VERIFY, 0xFFu, true, wrong_crc));
    assert_reply(CAN_UPDATE_CMD_VERIFY, CAN_UPDATE_ERR_CRC, 0u);

    /* Fail closed: metadata block untouched (still erased) -> image invalid. */
    for (uint32_t i = 0u; i < METADATA_BLOCK_SIZE; i++) {
        TEST_ASSERT_EQUAL_HEX8(0xFFu, s_flash[METADATA_ADDR + i]);
    }
    TEST_ASSERT_EQUAL(IMAGE_ERR_MAGIC, image_validate(s_flash));

    /* GO after a failed VERIFY stays refused. */
    TEST_ASSERT_TRUE(send_cmd(CAN_UPDATE_CMD_GO, 0xFFu, false, 0u));
    assert_reply(CAN_UPDATE_CMD_GO, CAN_UPDATE_ERR_STATE, 0u);
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

    /* Stream block 0 with the program call rigged to fail: mid-window frames
     * stay silent, the block-completing frame NAKs err 0x02. */
    s_fail_next_program = true;
    uint32_t off = 0u;
    while (off < FLASH_BLOCK_SIZE) {
        const uint32_t chunk = (FLASH_BLOCK_SIZE - off > 6u) ? 6u
                                                             : (FLASH_BLOCK_SIZE - off);
        uint8_t d[8];
        memset(d, 0xFF, sizeof(d));
        d[0] = CAN_UPDATE_CMD_DATA;
        d[1] = s_seq;
        memcpy(&d[2], &s_image[off], chunk);
        const bool replied = send_frame(d);
        if (off + chunk == FLASH_BLOCK_SIZE) {
            assert_reply(CAN_UPDATE_CMD_DATA, CAN_UPDATE_ERR_FLASH, 0u);
        } else {
            TEST_ASSERT_FALSE(replied);
        }
        s_seq++;
        off += chunk;
    }

    /* Window discarded: retransmit block 0 from seq 0 and finish the update. */
    s_seq = 0u;
    for (uint32_t start = 0u; start < TEST_IMAGE_SIZE; start += FLASH_BLOCK_SIZE) {
        stream_block(start);
    }
    const uint32_t image_crc = crc32_ieee(s_image, TEST_IMAGE_SIZE);
    TEST_ASSERT_TRUE(send_cmd(CAN_UPDATE_CMD_VERIFY, 0xFFu, true, image_crc));
    assert_reply(CAN_UPDATE_CMD_VERIFY, CAN_UPDATE_OK, 0u);
    TEST_ASSERT_EQUAL(IMAGE_VALID, image_validate(s_flash));
}

static void test_hello_restarts_session_mid_transfer(void)
{
    enter_transfer_phase();
    stream_block(0u);  /* one block in */

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
    const uint32_t image_crc = crc32_ieee(s_image, TEST_IMAGE_SIZE);
    TEST_ASSERT_TRUE(send_cmd(CAN_UPDATE_CMD_VERIFY, 0xFFu, true, image_crc));
    assert_reply(CAN_UPDATE_CMD_VERIFY, CAN_UPDATE_OK, 0u);
    TEST_ASSERT_EQUAL(IMAGE_VALID, image_validate(s_flash));

    TEST_ASSERT_TRUE(send_cmd(CAN_UPDATE_CMD_GO, 0xFFu, false, 0u));
    assert_reply(CAN_UPDATE_CMD_GO, CAN_UPDATE_OK, 0u);
    TEST_ASSERT_EQUAL(CAN_UPDATE_ACTION_RESET, s_action);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_frames_on_other_ids_are_ignored);
    RUN_TEST(test_hello_ack_carries_version_and_slot_size);
    RUN_TEST(test_hello_wrong_version_naks);
    RUN_TEST(test_happy_path_full_update);
    RUN_TEST(test_out_of_order_command_naks_state);
    RUN_TEST(test_go_before_verify_naks_state);
    RUN_TEST(test_size_out_of_bounds_naks);
    RUN_TEST(test_seq_gap_mid_window_rewinds_and_recovers);
    RUN_TEST(test_verify_wrong_crc_naks_and_leaves_metadata_erased);
    RUN_TEST(test_erase_failure_naks_flash_error);
    RUN_TEST(test_program_failure_naks_and_window_retransmit_recovers);
    RUN_TEST(test_hello_restarts_session_mid_transfer);
    RUN_TEST(test_malformed_dlc_naks_state);
    RUN_TEST(test_custom_cmd_id_is_matched_and_default_ignored);
    return UNITY_END();
}
