/* =============================================================================
 * test_can_flash_host.c — Host vs real node engine, RAM slot and fake clock
 * ============================================================================= */
#include <stdio.h>
#include <string.h>
#include "unity.h"
#include "can_flash_host.h"
#include "crc32.h"

#define SLOT_BYTES 0x1C0000u
#define IMAGE_BYTES (2u * CAN_UPDATE_WINDOW_SIZE + 1003u)
#define LARGE_BYTES (132u * CAN_UPDATE_WINDOW_SIZE + 19u)
#define QUEUE_FRAMES 64u
#define NODES 3u
#define NO_FAULT UINT32_MAX

static uint8_t s_image[SLOT_BYTES];
static uint8_t s_slot[SLOT_BYTES];
static can_update_state_t s_nodes[NODES];
static const uint16_t s_cmd[NODES] = { CAN_ID_HOST_TO_BOOT, 0x705u, 7u };
static const uint16_t s_reply[NODES] = { CAN_ID_BOOT_TO_HOST, 0x7ABu, 0x7CDu };
static char s_names[NODES][DFU_CONFIG_STR_MAX + 1u];
static uint8_t s_name_len[NODES];
static can_flash_host_frame_t s_queue[QUEUE_FRAMES];
static uint32_t s_head, s_tail, s_now, s_node_count, s_data_count, s_progress;
static uint32_t s_drop_data, s_drop_ack_window, s_duplicate_data, s_reset_data;
static uint32_t s_fail_programs, s_verify_count, s_commands[CAN_UPDATE_CMD_INFO + 1u];
static uint8_t s_drop_command_ack;
static bool s_corrupt, s_reject, s_activated, s_fail_send, s_bad_dlc;
static can_flash_host_t s_host;
static can_flash_host_result_t s_result;
static FILE *s_trace;

static bool erase_slot(void) { memset(s_slot, 0xFF, sizeof(s_slot)); s_activated = false; return true; }
static bool program_slot(uint32_t offset, const uint8_t *data, uint32_t len)
{
    if (s_fail_programs != 0u) { --s_fail_programs; return false; }
    TEST_ASSERT_TRUE(offset + len <= SLOT_BYTES);
    memcpy(&s_slot[offset], data, len);
    return true;
}
static bool read_slot(uint32_t offset, uint8_t *buf, uint32_t len)
{
    TEST_ASSERT_TRUE(offset + len <= SLOT_BYTES);
    memcpy(buf, &s_slot[offset], len);
    if (s_corrupt && offset == 0u) { buf[0] ^= 1u; }
    return true;
}
static bool commit_slot(void) { return !s_reject; }
static bool activate_slot(void) { s_activated = true; return true; }
static const can_update_hal_t s_hal = {
    SLOT_BYTES, erase_slot, program_slot, read_slot, commit_slot, activate_slot,
};

static void enqueue(uint16_t id, const uint8_t data[8])
{
    TEST_ASSERT_TRUE(s_tail - s_head < QUEUE_FRAMES);
    can_flash_host_frame_t *f = &s_queue[s_tail++ % QUEUE_FRAMES];
    memset(f, 0, sizeof(*f));
    f->id = id;
    f->dlc = s_bad_dlc ? 7u : CAN_FLASH_HOST_DLC;
    memcpy(f->data, data, sizeof(f->data));
}

static void dispatch(uint32_t node, const can_flash_host_frame_t *frame)
{
    if (frame->data[0] == CAN_UPDATE_CMD_INFO) {
        uint8_t header[8] = { CAN_UPDATE_REPLY_MARKER, CAN_UPDATE_CMD_INFO, 0u,
                             CAN_UPDATE_PROTOCOL_VERSION, 0xFFu, 0xFFu, 0xFFu, 0xFFu };
        header[2] = s_name_len[node];
        enqueue(s_reply[node], header);
        for (uint32_t off = 0u; off < DFU_CONFIG_STR_MAX; off += CAN_FLASH_HOST_DLC) {
            enqueue(s_reply[node], (const uint8_t *)&s_names[node][off]);
        }
        return;
    }
    uint8_t reply[8];
    can_update_action_t action;
    if (can_update_handle_frame(&s_nodes[node], frame->id, frame->dlc, frame->data, reply, &action)) {
        if (reply[2] == CAN_UPDATE_OK && reply[1] == CAN_UPDATE_CMD_DATA &&
            s_nodes[node].block_index - 1u == s_drop_ack_window) {
            s_drop_ack_window = NO_FAULT;
            return;
        }
        if (reply[2] == CAN_UPDATE_OK && reply[1] == s_drop_command_ack) {
            s_drop_command_ack = 0u;
            return;
        }
        enqueue(s_reply[node], reply);
    }
}

static can_flash_host_io_t bus_send(void *user, const can_flash_host_frame_t *f, uint32_t timeout)
{
    (void)user;
    TEST_ASSERT_EQUAL_UINT32(CAN_FLASH_HOST_SEND_MS, timeout);
    TEST_ASSERT_FALSE(f->extended);
    TEST_ASSERT_FALSE(f->remote);
    TEST_ASSERT_EQUAL_UINT8(CAN_FLASH_HOST_DLC, f->dlc);
    if (s_fail_send) { return CAN_FLASH_HOST_IO_ERROR; }
    if (s_trace != NULL) { TEST_ASSERT_EQUAL_UINT32(CAN_FLASH_HOST_DLC, fwrite(f->data, 1u, CAN_FLASH_HOST_DLC, s_trace)); }
    ++s_commands[f->data[0]];
    if (f->data[0] == CAN_UPDATE_CMD_VERIFY) { ++s_verify_count; }
    bool duplicate = false;
    if (f->data[0] == CAN_UPDATE_CMD_DATA) {
        const uint32_t index = s_data_count++;
        if (index == s_drop_data) { s_drop_data = NO_FAULT; return CAN_FLASH_HOST_IO_OK; }
        if (index == s_reset_data) { can_update_init(&s_nodes[0], &s_hal, s_cmd[0]); s_reset_data = NO_FAULT; }
        duplicate = index == s_duplicate_data;
    }
    for (uint32_t i = 0u; i < s_node_count; ++i) {
        if (f->id == s_cmd[i]) {
            dispatch(i, f);
            if (duplicate) { dispatch(i, f); }
        }
    }
    return CAN_FLASH_HOST_IO_OK;
}

static can_flash_host_io_t bus_receive(void *user, can_flash_host_frame_t *f, uint32_t timeout)
{
    (void)user;
    if (s_head == s_tail) { s_now += timeout; return CAN_FLASH_HOST_IO_TIMEOUT; }
    *f = s_queue[s_head++ % QUEUE_FRAMES];
    return CAN_FLASH_HOST_IO_OK;
}
static uint32_t clock_ms(void *user) { (void)user; return s_now; }
static bool read_image(void *user, uint32_t off, uint8_t *buf, uint32_t len)
{
    (void)user;
    if (off > SLOT_BYTES || len > SLOT_BYTES - off) { return false; }
    memcpy(buf, &s_image[off], len);
    return true;
}
static void progress(void *user, uint32_t bytes, uint32_t total)
{
    (void)user;
    TEST_ASSERT_TRUE(bytes <= total);
    TEST_ASSERT_TRUE(bytes >= s_progress);
    s_progress = bytes;
}

void setUp(void)
{
    s_head = s_tail = s_now = s_data_count = s_progress = s_verify_count = 0u;
    s_fail_programs = 0u;
    s_node_count = 1u;
    s_drop_data = s_drop_ack_window = s_duplicate_data = s_reset_data = NO_FAULT;
    s_drop_command_ack = 0u;
    s_corrupt = s_reject = s_activated = s_fail_send = s_bad_dlc = false;
    memset(s_commands, 0, sizeof(s_commands));
    memset(s_names, 0, sizeof(s_names));
    memset(s_name_len, 0, sizeof(s_name_len));
    for (uint32_t i = 0u; i < SLOT_BYTES; ++i) { s_image[i] = (uint8_t)((i * 37u) ^ (i >> 8u)); }
    for (uint32_t i = 0u; i < NODES; ++i) { can_update_init(&s_nodes[i], &s_hal, s_cmd[i]); }
    s_host = (can_flash_host_t){ .send = bus_send, .receive = bus_receive,
        .clock_ms = clock_ms, .read = read_image, .progress = progress };
}
void tearDown(void) {}

static can_flash_host_status_t flash(uint32_t size)
{
    return can_flash_host_flash(&s_host, s_cmd[0], s_reply[0], size, &s_result);
}
static void assert_clean(uint32_t size)
{
    TEST_ASSERT_EQUAL(CAN_FLASH_HOST_STATUS_OK, flash(size));
    TEST_ASSERT_EQUAL_MEMORY(s_image, s_slot, size);
    TEST_ASSERT_EQUAL_HEX32(crc32_ieee(s_image, size), s_result.crc32);
    TEST_ASSERT_TRUE(s_activated);
    TEST_ASSERT_EQUAL_UINT32(size, s_progress);
    TEST_ASSERT_EQUAL_UINT32(size, s_result.bytes);
}
static void test_clean_multi_window(void) { assert_clean(IMAGE_BYTES); }
static void test_large_sequence_wrap_rewind(void)
{
    s_drop_data = 130u * CAN_FLASH_HOST_FRAMES_PER_WINDOW + 13u;
    s_drop_ack_window = 129u;
    assert_clean(LARGE_BYTES);
    TEST_ASSERT_TRUE(s_data_count > 132u * CAN_FLASH_HOST_FRAMES_PER_WINDOW);
}
static void test_dropped_data(void) { s_drop_data = 400u; assert_clean(IMAGE_BYTES); }
static void test_lost_ack(void) { s_drop_ack_window = 0u; assert_clean(IMAGE_BYTES); }
static void test_duplicate_data(void) { s_duplicate_data = 400u; assert_clean(IMAGE_BYTES); }
static void test_final_ack_probe(void)
{
    s_drop_ack_window = 2u;
    assert_clean(IMAGE_BYTES);
    TEST_ASSERT_EQUAL_UINT32(1u, s_verify_count);
    TEST_ASSERT_EQUAL_UINT32(2u * CAN_FLASH_HOST_FRAMES_PER_WINDOW + (1003u + 5u) / 6u, s_data_count);
}
static void test_final_frame_lost_probe_state_retry(void)
{
    s_drop_data = 2u * CAN_FLASH_HOST_FRAMES_PER_WINDOW + (1003u + 5u) / 6u - 1u;
    assert_clean(IMAGE_BYTES);
    TEST_ASSERT_EQUAL_UINT32(2u, s_verify_count);
}
static void test_final_ack_probe_crc_failure(void)
{
    s_drop_ack_window = 2u;
    s_corrupt = true;
    TEST_ASSERT_EQUAL(CAN_FLASH_HOST_STATUS_NAK, flash(IMAGE_BYTES));
    TEST_ASSERT_EQUAL(CAN_FLASH_HOST_STEP_VERIFY, s_result.step);
    TEST_ASSERT_EQUAL(CAN_UPDATE_ERR_CRC, s_result.nak);
    TEST_ASSERT_FALSE(s_activated);
}
static void test_reset_then_fresh_session(void)
{
    s_reset_data = 400u;
    TEST_ASSERT_EQUAL(CAN_FLASH_HOST_STATUS_NAK, flash(IMAGE_BYTES));
    TEST_ASSERT_EQUAL(CAN_FLASH_HOST_STEP_DATA, s_result.step);
    TEST_ASSERT_EQUAL(CAN_UPDATE_ERR_STATE, s_result.nak);
    s_progress = 0u;
    assert_clean(IMAGE_BYTES);
}
static void test_crc_mismatch(void)
{
    s_corrupt = true;
    TEST_ASSERT_EQUAL(CAN_FLASH_HOST_STATUS_NAK, flash(IMAGE_BYTES));
    TEST_ASSERT_EQUAL(CAN_FLASH_HOST_STEP_VERIFY, s_result.step);
    TEST_ASSERT_EQUAL(CAN_UPDATE_ERR_CRC, s_result.nak);
    TEST_ASSERT_FALSE(s_activated);
}
static void test_image_rejected(void)
{
    s_reject = true;
    TEST_ASSERT_EQUAL(CAN_FLASH_HOST_STATUS_NAK, flash(IMAGE_BYTES));
    TEST_ASSERT_EQUAL(CAN_FLASH_HOST_STEP_VERIFY, s_result.step);
    TEST_ASSERT_EQUAL(CAN_UPDATE_ERR_IMAGE, s_result.nak);
    char text[160];
    can_flash_host_result_text(&s_result, text, sizeof(text));
    TEST_ASSERT_NOT_NULL(strstr(text, "VERIFY rejected: image rejected 0x06"));
    TEST_ASSERT_FALSE(s_activated);
}
static void test_oversized_size_nak(void)
{
    TEST_ASSERT_EQUAL(CAN_FLASH_HOST_STATUS_NAK, flash(SLOT_BYTES + 1u));
    TEST_ASSERT_EQUAL(CAN_FLASH_HOST_STEP_SIZE, s_result.step);
    TEST_ASSERT_EQUAL(CAN_UPDATE_ERR_SIZE, s_result.nak);
    TEST_ASSERT_EQUAL_UINT32(0u, s_commands[CAN_UPDATE_CMD_ERASE]);
}
static void test_absent_info_hello_timeouts(void)
{
    s_node_count = 0u;
    can_flash_host_info_t info;
    TEST_ASSERT_EQUAL(CAN_FLASH_HOST_STATUS_TIMEOUT, can_flash_host_info(&s_host, s_cmd[0], s_reply[0], &info));
    TEST_ASSERT_EQUAL_UINT32(CAN_FLASH_HOST_ATTEMPTS * (CAN_FLASH_HOST_ACK_MS + CAN_FLASH_HOST_QUIET_MS), s_now);
    s_now = 0u;
    TEST_ASSERT_EQUAL(CAN_FLASH_HOST_STATUS_TIMEOUT, flash(IMAGE_BYTES));
    TEST_ASSERT_EQUAL(CAN_FLASH_HOST_STEP_HELLO, s_result.step);
    TEST_ASSERT_EQUAL_UINT32(CAN_FLASH_HOST_ATTEMPTS * CAN_FLASH_HOST_ACK_MS, s_now);
}
static void test_three_node_scan(void)
{
    s_node_count = NODES;
    const char *const names[] = { "Legacy", "Sensor-Node-B", "Known-Device-C" };
    for (uint32_t i = 0u; i < NODES; ++i) {
        s_name_len[i] = (uint8_t)strlen(names[i]);
        memcpy(s_names[i], names[i], s_name_len[i]);
    }
    can_flash_host_info_t found[NODES];
    uint32_t count;
    TEST_ASSERT_EQUAL(CAN_FLASH_HOST_STATUS_OK, can_flash_host_scan(&s_host, CAN_FLASH_HOST_SCAN_FROM,
                      CAN_FLASH_HOST_SCAN_TO, found, NODES, &count));
    TEST_ASSERT_EQUAL_UINT32(NODES, count);
    for (uint32_t i = 0u; i < NODES; ++i) {
        const uint32_t n = NODES - 1u - i;
        TEST_ASSERT_EQUAL_UINT16(s_cmd[n], found[i].cmd_id);
        TEST_ASSERT_EQUAL_UINT16(s_reply[n], found[i].reply_id);
        TEST_ASSERT_EQUAL_STRING(s_names[n], found[i].name);
        TEST_ASSERT_EQUAL_UINT8(CAN_UPDATE_PROTOCOL_VERSION, found[i].protocol_version);
    }
    TEST_ASSERT_EQUAL_UINT32(38u, s_commands[CAN_UPDATE_CMD_INFO]);
}
static void test_info_all_name_lengths(void)
{
    for (uint8_t len = 0u; len <= DFU_CONFIG_STR_MAX; ++len) {
        memset(s_names[0], 0, sizeof(s_names[0]));
        memset(s_names[0], 'A', len);
        s_name_len[0] = len;
        can_flash_host_info_t info;
        TEST_ASSERT_EQUAL(CAN_FLASH_HOST_STATUS_OK, can_flash_host_info(&s_host, s_cmd[0], s_reply[0], &info));
        TEST_ASSERT_EQUAL_UINT8(len, info.name_len);
        TEST_ASSERT_EQUAL_STRING(s_names[0], info.name);
    }
}
static void test_scan_capacity(void)
{
    s_node_count = NODES;
    can_flash_host_info_t found[1];
    uint32_t count;
    TEST_ASSERT_EQUAL(CAN_FLASH_HOST_STATUS_CAPACITY, can_flash_host_scan(&s_host, CAN_FLASH_HOST_SCAN_FROM,
                      CAN_FLASH_HOST_SCAN_TO, found, 1u, &count));
    TEST_ASSERT_EQUAL_UINT32(1u, count);
}
static void test_flash_failure_retry_limit(void)
{
    s_fail_programs = CAN_FLASH_HOST_ATTEMPTS;
    TEST_ASSERT_EQUAL(CAN_FLASH_HOST_STATUS_TIMEOUT, flash(IMAGE_BYTES));
    TEST_ASSERT_EQUAL(CAN_FLASH_HOST_STEP_DATA, s_result.step);
    TEST_ASSERT_EQUAL_UINT32(CAN_FLASH_HOST_ATTEMPTS, s_result.attempts);
    TEST_ASSERT_EQUAL(CAN_UPDATE_ERR_FLASH, s_result.nak);
    TEST_ASSERT_FALSE(s_activated);
}
static void test_flash_failure_recovers(void) { s_fail_programs = 1u; assert_clean(IMAGE_BYTES); }
static void test_lost_size_ack_is_strict_state_failure(void)
{
    s_drop_command_ack = CAN_UPDATE_CMD_SIZE;
    TEST_ASSERT_EQUAL(CAN_FLASH_HOST_STATUS_NAK, flash(IMAGE_BYTES));
    TEST_ASSERT_EQUAL(CAN_FLASH_HOST_STEP_SIZE, s_result.step);
    TEST_ASSERT_EQUAL(CAN_UPDATE_ERR_STATE, s_result.nak);
    TEST_ASSERT_FALSE(s_activated);
}
static void test_transport_error(void)
{
    s_fail_send = true;
    TEST_ASSERT_EQUAL(CAN_FLASH_HOST_STATUS_TRANSPORT, flash(IMAGE_BYTES));
    TEST_ASSERT_EQUAL(CAN_FLASH_HOST_STEP_HELLO, s_result.step);
}
static void test_malformed_reply(void)
{
    s_bad_dlc = true;
    TEST_ASSERT_EQUAL(CAN_FLASH_HOST_STATUS_PROTOCOL, flash(IMAGE_BYTES));
}
static void test_clock_wrap(void) { s_now = UINT32_MAX - 10u; assert_clean(IMAGE_BYTES); }

int main(int argc, char **argv)
{
    UNITY_BEGIN();
    if (argc == 2) {
#ifdef _MSC_VER
        if (fopen_s(&s_trace, argv[1], "wb") != 0) { return 2; }
#else
        s_trace = fopen(argv[1], "wb");
#endif
        if (s_trace == NULL) { return 2; }
        RUN_TEST(test_clean_multi_window);
        if (fclose(s_trace) != 0) { return 2; }
        s_trace = NULL;
        return UNITY_END();
    }
    RUN_TEST(test_clean_multi_window);
    RUN_TEST(test_large_sequence_wrap_rewind);
    RUN_TEST(test_dropped_data);
    RUN_TEST(test_lost_ack);
    RUN_TEST(test_duplicate_data);
    RUN_TEST(test_final_ack_probe);
    RUN_TEST(test_final_frame_lost_probe_state_retry);
    RUN_TEST(test_final_ack_probe_crc_failure);
    RUN_TEST(test_reset_then_fresh_session);
    RUN_TEST(test_crc_mismatch);
    RUN_TEST(test_image_rejected);
    RUN_TEST(test_oversized_size_nak);
    RUN_TEST(test_absent_info_hello_timeouts);
    RUN_TEST(test_three_node_scan);
    RUN_TEST(test_info_all_name_lengths);
    RUN_TEST(test_scan_capacity);
    RUN_TEST(test_flash_failure_retry_limit);
    RUN_TEST(test_flash_failure_recovers);
    RUN_TEST(test_lost_size_ack_is_strict_state_failure);
    RUN_TEST(test_transport_error);
    RUN_TEST(test_malformed_reply);
    RUN_TEST(test_clock_wrap);
    return UNITY_END();
}
