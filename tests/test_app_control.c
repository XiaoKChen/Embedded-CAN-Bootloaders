/* =============================================================================
 * test_app_control.c — App-control extension codec (APP_INFO / ENTER_UPDATER)
 * =============================================================================
 * Golden vectors (shared with tools/can-flash check_app_control_vector), the
 * request parser's fail-closed paths, reply classification against the REAL
 * updater engine's NAK, and APP_INFO decode rejecting partial/foreign bursts.
 * ============================================================================= */

#include <string.h>

#include "unity.h"

#include "app_control.h"
#include "can_update.h"

static const uint8_t k_enter_request[8] = { 0x09, 0x01, 0xFE, 0xCA, 0x07, 0xB0, 0xFF, 0xFF };
static const uint8_t k_app_info_request[8] = { 0x08, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
/* APP_INFO reply for version "1.2.3", project "node_app" (check byte 0x6A). */
static const uint8_t k_info_reply[APP_CONTROL_INFO_BYTES] = {
    0x79, 0x08, 0x00, 0xA5, 0x01, 0x05, 0x08, 0x6A,
    0x31, 0x2E, 0x32, 0x2E, 0x33, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x6E, 0x6F, 0x64, 0x65, 0x5F, 0x61, 0x70, 0x70,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static bool slot_ok(void) { return true; }
static bool slot_program(uint32_t offset, const uint8_t *data, uint32_t len)
{
    (void)offset; (void)data; (void)len;
    return true;
}
static bool slot_read(uint32_t offset, uint8_t *buf, uint32_t len)
{
    (void)offset;
    memset(buf, 0xFF, len);
    return true;
}
static const can_update_hal_t s_hal = { 0x10000u, slot_ok, slot_program, slot_read, slot_ok, slot_ok };

void setUp(void) {}
void tearDown(void) {}

static void test_request_golden_vectors(void)
{
    uint8_t frame[8];
    app_control_encode_request(CAN_UPDATE_CMD_ENTER_UPDATER, frame);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(k_enter_request, frame, 8);
    app_control_encode_request(CAN_UPDATE_CMD_APP_INFO, frame);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(k_app_info_request, frame, 8);
}

static void test_info_golden_vector_round_trip(void)
{
    uint8_t frames[APP_CONTROL_INFO_BYTES];
    app_control_encode_info("1.2.3", "node_app", frames);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(k_info_reply, frames, APP_CONTROL_INFO_BYTES);

    app_control_info_t info;
    TEST_ASSERT_TRUE(app_control_decode_info(frames, &info));
    TEST_ASSERT_EQUAL_UINT8(CAN_UPDATE_PROTOCOL_VERSION, info.protocol_version);
    TEST_ASSERT_EQUAL_STRING("1.2.3", info.version);
    TEST_ASSERT_EQUAL_STRING("node_app", info.project_name);
}

static void test_info_every_length_and_truncation(void)
{
    char text[APP_CONTROL_STR_MAX + 8u];
    for (uint32_t len = 0u; len < sizeof(text); len++) {
        memset(text, 'v', len);
        text[len] = '\0';
        uint8_t frames[APP_CONTROL_INFO_BYTES];
        app_control_encode_info(text, (len % 2u == 0u) ? NULL : text, frames);
        app_control_info_t info;
        TEST_ASSERT_TRUE(app_control_decode_info(frames, &info));
        const uint32_t expect = (len > APP_CONTROL_STR_MAX) ? APP_CONTROL_STR_MAX : len;
        TEST_ASSERT_EQUAL_UINT8(expect, info.version_len);
        TEST_ASSERT_EQUAL_UINT32(expect, (uint32_t)strlen(info.version));
        TEST_ASSERT_EQUAL_UINT8((len % 2u == 0u) ? 0u : expect, info.project_len);
    }
}

static void test_parse_request(void)
{
    TEST_ASSERT_EQUAL(APP_CONTROL_REQ_APP_INFO, app_control_parse_request(8u, k_app_info_request));
    TEST_ASSERT_EQUAL(APP_CONTROL_REQ_ENTER_UPDATER, app_control_parse_request(8u, k_enter_request));

    /* Updater-only commands are never answered by an app. */
    const uint8_t updater_cmds[] = { CAN_UPDATE_CMD_HELLO, CAN_UPDATE_CMD_ERASE, CAN_UPDATE_CMD_DATA,
                                     CAN_UPDATE_CMD_SIZE, CAN_UPDATE_CMD_VERIFY, CAN_UPDATE_CMD_GO,
                                     CAN_UPDATE_CMD_INFO, 0x00u, 0x0Au, 0xFFu };
    for (uint32_t i = 0u; i < sizeof(updater_cmds); i++) {
        uint8_t frame[8];
        memcpy(frame, k_enter_request, sizeof(frame));
        frame[0] = updater_cmds[i];
        TEST_ASSERT_EQUAL(APP_CONTROL_REQ_NONE, app_control_parse_request(8u, frame));
    }
}

static void test_parse_enter_fails_closed(void)
{
    uint8_t frame[8];
    TEST_ASSERT_EQUAL(APP_CONTROL_REQ_MALFORMED, app_control_parse_request(7u, k_enter_request));
    TEST_ASSERT_EQUAL(APP_CONTROL_REQ_MALFORMED, app_control_parse_request(0u, k_app_info_request));
    for (uint32_t byte = 1u; byte <= 5u; byte++) {
        memcpy(frame, k_enter_request, sizeof(frame));
        frame[byte] ^= 0x01u;
        TEST_ASSERT_EQUAL(APP_CONTROL_REQ_MALFORMED, app_control_parse_request(8u, frame));
    }
}

static void test_classify_against_real_updater(void)
{
    can_update_state_t engine;
    can_update_init(&engine, &s_hal, CAN_ID_HOST_TO_BOOT);
    const uint8_t cmds[] = { CAN_UPDATE_CMD_APP_INFO, CAN_UPDATE_CMD_ENTER_UPDATER };
    for (uint32_t i = 0u; i < sizeof(cmds); i++) {
        uint8_t request[8];
        uint8_t reply[8];
        can_update_action_t action;
        app_control_encode_request(cmds[i], request);
        TEST_ASSERT_TRUE(can_update_handle_frame(&engine, CAN_ID_HOST_TO_BOOT, 8u, request, reply, &action));
        TEST_ASSERT_EQUAL(APP_CONTROL_REPLY_UPDATER_NAK, app_control_classify_reply(cmds[i], reply));
        TEST_ASSERT_EQUAL(CAN_UPDATE_ACTION_NONE, action);

        app_control_encode_reply(cmds[i], CAN_UPDATE_OK, reply);
        TEST_ASSERT_EQUAL(APP_CONTROL_REPLY_APP_ACK, app_control_classify_reply(cmds[i], reply));
        app_control_encode_reply(cmds[i], CAN_UPDATE_ERR_STATE, reply);
        TEST_ASSERT_EQUAL(APP_CONTROL_REPLY_APP_NAK, app_control_classify_reply(cmds[i], reply));
        TEST_ASSERT_EQUAL(APP_CONTROL_REPLY_FOREIGN, app_control_classify_reply((uint8_t)(cmds[i] ^ 1u), reply));
    }

    /* An ACK without the app marker (e.g. some other device's ACK) is foreign. */
    const uint8_t plain_ack[8] = { 0x79, 0x09, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF };
    TEST_ASSERT_EQUAL(APP_CONTROL_REPLY_FOREIGN, app_control_classify_reply(CAN_UPDATE_CMD_ENTER_UPDATER, plain_ack));
    const uint8_t other_nak[8] = { 0x79, 0x09, 0x02, 0x00, 0xFF, 0xFF, 0xFF, 0xFF };
    TEST_ASSERT_EQUAL(APP_CONTROL_REPLY_FOREIGN, app_control_classify_reply(CAN_UPDATE_CMD_ENTER_UPDATER, other_nak));
}

static void assert_rejected(uint32_t index, uint8_t value)
{
    uint8_t frames[APP_CONTROL_INFO_BYTES];
    memcpy(frames, k_info_reply, sizeof(frames));
    frames[index] = value;
    app_control_info_t info;
    memset(&info, 0x5A, sizeof(info));
    TEST_ASSERT_FALSE(app_control_decode_info(frames, &info));
    TEST_ASSERT_EQUAL_HEX8(0x5Au, info.protocol_version);  /* out untouched */
}

static void test_decode_rejects_partial_or_foreign(void)
{
    assert_rejected(0u, 0x7Au);   /* not a reply                   */
    assert_rejected(1u, 0x07u);   /* updater INFO header           */
    assert_rejected(3u, 0x00u);   /* no app marker                 */
    assert_rejected(7u, 0x6Bu);   /* check byte                    */
    assert_rejected(5u, 0x06u);   /* length disagrees with check   */
    assert_rejected(13u, 0x20u);  /* garbage past the version      */
    assert_rejected(60u, 0x01u);  /* garbage past the project name */

    /* A lost frame shifts the burst: frame 1 repeated in place of frame 2. */
    uint8_t frames[APP_CONTROL_INFO_BYTES];
    memcpy(frames, k_info_reply, sizeof(frames));
    memcpy(&frames[16], &frames[8], 8u);
    app_control_info_t info;
    TEST_ASSERT_FALSE(app_control_decode_info(frames, &info));

    /* Lengths above 32 with a matching check byte still fail closed. */
    uint8_t long_frames[APP_CONTROL_INFO_BYTES];
    app_control_encode_info("abc", "def", long_frames);
    long_frames[5] = APP_CONTROL_STR_MAX + 1u;
    TEST_ASSERT_FALSE(app_control_decode_info(long_frames, &info));

    /* An embedded NUL inside the declared length. */
    app_control_encode_info("abc", "def", long_frames);
    long_frames[5] = 4u;
    TEST_ASSERT_FALSE(app_control_decode_info(long_frames, &info));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_request_golden_vectors);
    RUN_TEST(test_info_golden_vector_round_trip);
    RUN_TEST(test_info_every_length_and_truncation);
    RUN_TEST(test_parse_request);
    RUN_TEST(test_parse_enter_fails_closed);
    RUN_TEST(test_classify_against_real_updater);
    RUN_TEST(test_decode_rejects_partial_or_foreign);
    return UNITY_END();
}
