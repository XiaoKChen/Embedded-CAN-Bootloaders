/* =============================================================================
 * test_can_flash_host.c — Host vs real node engine, RAM slot and fake clock
 * =============================================================================
 * A node is either parked (the real can_update engine, plus INFO) or running
 * its app (s_app_mode: answers only APP_INFO / ENTER_UPDATER through the same
 * app_control codec as examples/node_app; ENTER "restarts" it into the
 * updater).
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
#define NODES 3u      /* the three-node updater scans use nodes 0..2 */
#define NODES_MAX 5u  /* nodes 3..4 add custom IDs for mixed app/updater scans */
#define NO_FAULT UINT32_MAX

static uint8_t s_image[SLOT_BYTES];
static uint8_t s_slot[SLOT_BYTES];
static can_update_state_t s_nodes[NODES_MAX];
static const uint16_t s_cmd[NODES_MAX] = { CAN_ID_HOST_TO_BOOT, 0x705u, 7u, 0x05u, 0x701u };
static const uint16_t s_reply[NODES_MAX] = { CAN_ID_BOOT_TO_HOST, 0x7ABu, 0x7CDu, 0x06u, 0x702u };
static char s_names[NODES_MAX][DFU_CONFIG_STR_MAX + 1u];
static uint8_t s_name_len[NODES_MAX];
static can_flash_host_frame_t s_queue[QUEUE_FRAMES];
static uint32_t s_head, s_tail, s_now, s_node_count, s_data_count, s_progress;
static uint32_t s_drop_data, s_drop_ack_window, s_duplicate_data, s_reset_data;
static uint32_t s_fail_programs, s_verify_count, s_commands[UINT8_MAX + 1u];
static uint8_t s_drop_command_ack;
static bool s_corrupt, s_reject, s_activated, s_fail_send, s_bad_dlc;
static bool s_app_mode[NODES_MAX], s_app_ext_noise, s_app_noise, s_app_corrupt, s_drop_enter_ack, s_app_refuse;
static uint32_t s_app_drop_frame, s_app_restarts;
#define NOISE_ID 0x123u
#define APP_VERSION "v2.4.1-node"
#define APP_PROJECT "node_app"
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

/* examples/node_app's CAN service in miniature. */
static void dispatch_app(uint32_t node, const can_flash_host_frame_t *frame)
{
    uint8_t reply[8];
    switch (app_control_parse_request(frame->dlc, frame->data)) {
    case APP_CONTROL_REQ_APP_INFO: {
        uint8_t frames[APP_CONTROL_INFO_BYTES];
        app_control_encode_info(APP_VERSION, APP_PROJECT, frames);
        if (s_app_corrupt) { frames[10] ^= 1u; }
        if (s_app_ext_noise) {  /* valid-looking header that is not a standard data frame */
            enqueue(s_reply[node], frames);
            s_queue[(s_tail - 1u) % QUEUE_FRAMES].extended = true;
            enqueue(s_reply[node], frames);
            s_queue[(s_tail - 1u) % QUEUE_FRAMES].remote = true;
            enqueue(s_reply[node], frames);
            s_queue[(s_tail - 1u) % QUEUE_FRAMES].error = true;
        }
        for (uint32_t i = 0u; i < APP_CONTROL_INFO_FRAMES; ++i) {
            if (s_app_noise) {
                const uint8_t noise[8] = { CAN_UPDATE_REPLY_MARKER, CAN_UPDATE_CMD_APP_INFO, 0, 0, 0, 0, 0, 0 };
                enqueue(NOISE_ID, noise);
            }
            if (i == s_app_drop_frame) { s_app_drop_frame = NO_FAULT; continue; }
            enqueue(s_reply[node], &frames[i * 8u]);
        }
        return;
    }
    case APP_CONTROL_REQ_ENTER_UPDATER:
        if (s_app_refuse) {
            app_control_encode_reply(CAN_UPDATE_CMD_ENTER_UPDATER, CAN_UPDATE_ERR_FLASH, reply);
            enqueue(s_reply[node], reply);
            return;
        }
        app_control_encode_reply(CAN_UPDATE_CMD_ENTER_UPDATER, CAN_UPDATE_OK, reply);
        if (!s_drop_enter_ack) { enqueue(s_reply[node], reply); }
        s_drop_enter_ack = false;
        s_app_mode[node] = false;  /* clean restart into the updater */
        can_update_init(&s_nodes[node], &s_hal, s_cmd[node]);
        ++s_app_restarts;
        return;
    case APP_CONTROL_REQ_MALFORMED:
        app_control_encode_reply(frame->data[0], CAN_UPDATE_ERR_STATE, reply);
        enqueue(s_reply[node], reply);
        return;
    default:
        return;  /* HELLO, INFO, ...: an app never answers updater commands */
    }
}

static void dispatch(uint32_t node, const can_flash_host_frame_t *frame)
{
    if (s_app_mode[node]) {
        dispatch_app(node, frame);
        return;
    }
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
    memset(s_app_mode, 0, sizeof(s_app_mode));
    s_app_ext_noise = s_app_noise = s_app_corrupt = s_drop_enter_ack = s_app_refuse = false;
    s_app_drop_frame = NO_FAULT;
    s_app_restarts = 0u;
    memset(s_commands, 0, sizeof(s_commands));
    memset(s_names, 0, sizeof(s_names));
    memset(s_name_len, 0, sizeof(s_name_len));
    for (uint32_t i = 0u; i < SLOT_BYTES; ++i) { s_image[i] = (uint8_t)((i * 37u) ^ (i >> 8u)); }
    for (uint32_t i = 0u; i < NODES_MAX; ++i) { can_update_init(&s_nodes[i], &s_hal, s_cmd[i]); }
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

static can_flash_host_status_t app_info(app_control_info_t *info)
{
    return can_flash_host_app_info(&s_host, s_cmd[0], s_reply[0], info);
}
static can_flash_host_status_t enter_updater(void)
{
    return can_flash_host_enter_updater(&s_host, s_cmd[0], s_reply[0]);
}
static void test_app_info_running_app(void)
{
    s_app_mode[0] = true;
    s_app_noise = true;  /* other traffic interleaved on a live bus */
    app_control_info_t info;
    TEST_ASSERT_EQUAL(CAN_FLASH_HOST_STATUS_OK, app_info(&info));
    TEST_ASSERT_EQUAL_STRING(APP_VERSION, info.version);
    TEST_ASSERT_EQUAL_STRING(APP_PROJECT, info.project_name);
    TEST_ASSERT_EQUAL_UINT8(CAN_UPDATE_PROTOCOL_VERSION, info.protocol_version);
    TEST_ASSERT_EQUAL_UINT32(1u, s_host.result.attempts);
    TEST_ASSERT_EQUAL(CAN_FLASH_HOST_STEP_APP_INFO, s_host.result.step);
}
static void test_app_ignores_updater_commands(void)
{
    s_app_mode[0] = true;
    can_flash_host_info_t info;
    TEST_ASSERT_EQUAL(CAN_FLASH_HOST_STATUS_TIMEOUT, can_flash_host_info(&s_host, s_cmd[0], s_reply[0], &info));
    TEST_ASSERT_EQUAL(CAN_FLASH_HOST_STATUS_TIMEOUT, flash(IMAGE_BYTES));
    TEST_ASSERT_EQUAL_UINT32(0u, s_tail);  /* the app never transmitted */
}
static void test_app_info_updater_nak(void)
{
    app_control_info_t info;
    TEST_ASSERT_EQUAL(CAN_FLASH_HOST_STATUS_IN_UPDATER, app_info(&info));
    TEST_ASSERT_EQUAL(CAN_UPDATE_ERR_STATE, s_host.result.nak);
    TEST_ASSERT_EQUAL(CAN_UPDATE_PHASE_IDLE, s_nodes[0].phase);
    char text[160];
    can_flash_host_result_text(&s_host.result, text, sizeof(text));
    TEST_ASSERT_NOT_NULL(strstr(text, "APP_INFO: node is in its updater"));
    assert_clean(IMAGE_BYTES);  /* the probe left the updater fully usable */
}
static void test_app_info_timeout(void)
{
    s_node_count = 0u;
    app_control_info_t info;
    TEST_ASSERT_EQUAL(CAN_FLASH_HOST_STATUS_TIMEOUT, app_info(&info));
    TEST_ASSERT_EQUAL_UINT32(CAN_FLASH_HOST_APP_ATTEMPTS * CAN_FLASH_HOST_APP_MS, s_now);
    TEST_ASSERT_EQUAL_UINT32(CAN_FLASH_HOST_APP_ATTEMPTS, s_commands[CAN_UPDATE_CMD_APP_INFO]);
}
static void test_app_info_malformed(void)
{
    s_app_mode[0] = true;
    s_app_corrupt = true;
    app_control_info_t info;
    TEST_ASSERT_EQUAL(CAN_FLASH_HOST_STATUS_PROTOCOL, app_info(&info));
    TEST_ASSERT_EQUAL_UINT32(CAN_FLASH_HOST_APP_ATTEMPTS, s_host.result.attempts);
    TEST_ASSERT_EQUAL_STRING("", info.version);
}
static void test_app_info_partial_burst_retried(void)
{
    s_app_mode[0] = true;
    s_app_drop_frame = 4u;
    app_control_info_t info;
    TEST_ASSERT_EQUAL(CAN_FLASH_HOST_STATUS_OK, app_info(&info));
    TEST_ASSERT_EQUAL_UINT32(2u, s_host.result.attempts);
    TEST_ASSERT_EQUAL_STRING(APP_PROJECT, info.project_name);
}
static void test_enter_updater_then_flash(void)
{
    s_app_mode[0] = true;
    TEST_ASSERT_EQUAL(CAN_FLASH_HOST_STATUS_OK, enter_updater());
    TEST_ASSERT_EQUAL(CAN_FLASH_HOST_STEP_ENTER_UPDATER, s_host.result.step);
    TEST_ASSERT_EQUAL_HEX8(CAN_UPDATE_APP_MARKER, s_host.result.arg);
    TEST_ASSERT_EQUAL_UINT32(1u, s_app_restarts);
    can_flash_host_info_t info;
    TEST_ASSERT_EQUAL(CAN_FLASH_HOST_STATUS_OK, can_flash_host_info(&s_host, s_cmd[0], s_reply[0], &info));
    assert_clean(IMAGE_BYTES);
}
static void test_enter_updater_lost_ack(void)
{
    s_app_mode[0] = true;
    s_drop_enter_ack = true;
    TEST_ASSERT_EQUAL(CAN_FLASH_HOST_STATUS_IN_UPDATER, enter_updater());
    TEST_ASSERT_EQUAL_UINT32(2u, s_host.result.attempts);
    TEST_ASSERT_EQUAL_UINT32(1u, s_app_restarts);
}
static void test_enter_updater_already_parked(void)
{
    TEST_ASSERT_EQUAL(CAN_FLASH_HOST_STATUS_IN_UPDATER, enter_updater());
    TEST_ASSERT_EQUAL_UINT32(1u, s_host.result.attempts);
    TEST_ASSERT_EQUAL_UINT32(0u, s_app_restarts);
}
static void test_enter_updater_refused(void)
{
    s_app_mode[0] = true;
    s_app_refuse = true;
    TEST_ASSERT_EQUAL(CAN_FLASH_HOST_STATUS_NAK, enter_updater());
    TEST_ASSERT_EQUAL(CAN_UPDATE_ERR_FLASH, s_host.result.nak);
    TEST_ASSERT_TRUE(s_app_mode[0]);
}
static void test_enter_updater_timeout(void)
{
    s_node_count = 0u;
    TEST_ASSERT_EQUAL(CAN_FLASH_HOST_STATUS_TIMEOUT, enter_updater());
    TEST_ASSERT_EQUAL_UINT32(CAN_FLASH_HOST_APP_ATTEMPTS * CAN_FLASH_HOST_APP_MS, s_now);
}
static void test_enter_updater_malformed(void)
{
    s_app_mode[0] = true;
    s_app_refuse = true;
    s_bad_dlc = true;
    TEST_ASSERT_EQUAL(CAN_FLASH_HOST_STATUS_PROTOCOL, enter_updater());
    TEST_ASSERT_EQUAL_UINT32(CAN_FLASH_HOST_APP_ATTEMPTS, s_host.result.attempts);
}
static void test_app_control_invalid_args(void)
{
    app_control_info_t info;
    TEST_ASSERT_EQUAL(CAN_FLASH_HOST_STATUS_INVALID_ARG, can_flash_host_app_info(&s_host, 0x800u, 1u, &info));
    TEST_ASSERT_EQUAL(CAN_FLASH_HOST_STATUS_INVALID_ARG, can_flash_host_app_info(&s_host, 1u, 1u, NULL));
    TEST_ASSERT_EQUAL(CAN_FLASH_HOST_STATUS_INVALID_ARG, can_flash_host_enter_updater(NULL, 1u, 1u));
    TEST_ASSERT_EQUAL(CAN_FLASH_HOST_STATUS_INVALID_ARG, can_flash_host_enter_updater(&s_host, 1u, 0x800u));
    TEST_ASSERT_EQUAL_UINT32(0u, s_commands[CAN_UPDATE_CMD_APP_INFO] + s_commands[CAN_UPDATE_CMD_ENTER_UPDATER]);
}

static void name_node(uint32_t node, const char *name)
{
    s_name_len[node] = (uint8_t)strlen(name);
    memcpy(s_names[node], name, s_name_len[node]);
}
/* Bus: updater 0x79E, app 0x705, updater 0x07, app 0x05, updater 0x701. */
static void mixed_bus(void)
{
    s_node_count = NODES_MAX;
    s_app_mode[1] = true;
    s_app_mode[3] = true;
    name_node(0u, "Legacy");
    name_node(2u, "Known-C");
    name_node(4u, "Custom-E");
}
static void assert_updater(const can_flash_host_node_t *n, uint32_t node)
{
    TEST_ASSERT_EQUAL(CAN_FLASH_HOST_NODE_UPDATER, n->mode);
    TEST_ASSERT_EQUAL_UINT16(s_cmd[node], n->cmd_id);
    TEST_ASSERT_EQUAL_UINT16(s_reply[node], n->reply_id);
    TEST_ASSERT_EQUAL_UINT8(CAN_UPDATE_PROTOCOL_VERSION, n->protocol_version);
    TEST_ASSERT_EQUAL_UINT8(s_name_len[node], n->name_len);
    TEST_ASSERT_EQUAL_STRING(s_names[node], n->name);
    TEST_ASSERT_EQUAL_STRING("", n->project);
    TEST_ASSERT_EQUAL_STRING("", n->version);
}
static void assert_app(const can_flash_host_node_t *n, uint32_t node)
{
    TEST_ASSERT_EQUAL(CAN_FLASH_HOST_NODE_APP, n->mode);
    TEST_ASSERT_EQUAL_UINT16(s_cmd[node], n->cmd_id);
    TEST_ASSERT_EQUAL_UINT16(s_reply[node], n->reply_id);
    TEST_ASSERT_EQUAL_UINT8(CAN_UPDATE_PROTOCOL_VERSION, n->protocol_version);
    TEST_ASSERT_EQUAL_STRING(APP_PROJECT, n->project);
    TEST_ASSERT_EQUAL_STRING(APP_VERSION, n->version);
    TEST_ASSERT_EQUAL_UINT8(0u, n->name_len);
    TEST_ASSERT_EQUAL_STRING("", n->name);
}
static can_flash_host_status_t discover(can_flash_host_node_t *found, uint32_t max, uint32_t *count)
{
    return can_flash_host_discover(&s_host, CAN_FLASH_HOST_SCAN_FROM, CAN_FLASH_HOST_SCAN_TO, found, max, count);
}
static void assert_mixed_found(const can_flash_host_node_t found[NODES_MAX])
{
    assert_app(&found[0], 3u);      /* 0x05 / 0x06 */
    assert_updater(&found[1], 2u);  /* 0x07 / 0x7CD */
    assert_updater(&found[2], 4u);  /* 0x701 / 0x702 */
    assert_app(&found[3], 1u);      /* 0x705 / 0x7AB */
    assert_updater(&found[4], 0u);  /* 0x79E / 0x79F */
}
static void test_discover_mixed_apps_and_updaters(void)
{
    mixed_bus();
    can_flash_host_node_t found[NODES_MAX];
    uint32_t count;
    TEST_ASSERT_EQUAL(CAN_FLASH_HOST_STATUS_OK, discover(found, NODES_MAX, &count));
    TEST_ASSERT_EQUAL_UINT32(NODES_MAX, count);
    assert_mixed_found(found);
    /* One APP_INFO probe per ID; INFO only follows an updater's NAK. */
    TEST_ASSERT_EQUAL_UINT32(38u, s_commands[CAN_UPDATE_CMD_APP_INFO]);
    TEST_ASSERT_EQUAL_UINT32(3u, s_commands[CAN_UPDATE_CMD_INFO]);
    /* Only silent IDs consume a window: no slower than a plain scan. */
    TEST_ASSERT_EQUAL_UINT32((38u - NODES_MAX) * CAN_FLASH_HOST_SCAN_MS, s_now);
    TEST_ASSERT_TRUE(s_now <= 38u * CAN_FLASH_HOST_SCAN_MS);
}
static void test_discover_silence_costs_one_window_per_id(void)
{
    can_flash_host_node_t found[1];
    uint32_t count = 99u;
    s_node_count = 0u;
    TEST_ASSERT_EQUAL(CAN_FLASH_HOST_STATUS_OK, discover(found, 1u, &count));
    TEST_ASSERT_EQUAL_UINT32(0u, count);
    TEST_ASSERT_EQUAL_UINT32(38u * CAN_FLASH_HOST_SCAN_MS, s_now);
}
static void test_discover_ignores_non_standard_and_foreign_frames(void)
{
    mixed_bus();
    s_app_ext_noise = true;
    s_app_noise = true;
    can_flash_host_node_t found[NODES_MAX];
    uint32_t count;
    TEST_ASSERT_EQUAL(CAN_FLASH_HOST_STATUS_OK, discover(found, NODES_MAX, &count));
    TEST_ASSERT_EQUAL_UINT32(NODES_MAX, count);
    assert_mixed_found(found);
}
static void test_discover_skips_corrupt_and_partial_app_bursts(void)
{
    mixed_bus();
    s_app_corrupt = true;
    can_flash_host_node_t found[NODES_MAX];
    uint32_t count;
    TEST_ASSERT_EQUAL(CAN_FLASH_HOST_STATUS_OK, discover(found, NODES_MAX, &count));
    TEST_ASSERT_EQUAL_UINT32(3u, count);  /* the three updaters only */
    for (uint32_t i = 0u; i < count; ++i) { TEST_ASSERT_EQUAL(CAN_FLASH_HOST_NODE_UPDATER, found[i].mode); }
    s_app_corrupt = false;
    s_app_drop_frame = 4u;
    TEST_ASSERT_EQUAL(CAN_FLASH_HOST_STATUS_OK, discover(found, NODES_MAX, &count));
    TEST_ASSERT_EQUAL_UINT32(4u, count);  /* the first app burst lost a frame */
}
static void test_discover_capacity_and_invalid_args(void)
{
    mixed_bus();
    can_flash_host_node_t found[2];
    uint32_t count;
    TEST_ASSERT_EQUAL(CAN_FLASH_HOST_STATUS_CAPACITY, discover(found, 2u, &count));
    TEST_ASSERT_EQUAL_UINT32(2u, count);
    TEST_ASSERT_EQUAL(CAN_FLASH_HOST_STATUS_INVALID_ARG, can_flash_host_discover(&s_host, 5u, 4u, found, 2u, &count));
    TEST_ASSERT_EQUAL(CAN_FLASH_HOST_STATUS_INVALID_ARG, can_flash_host_discover(&s_host, 0u, 0x800u, found, 2u, &count));
    TEST_ASSERT_EQUAL(CAN_FLASH_HOST_STATUS_INVALID_ARG, can_flash_host_discover(&s_host, 0u, 1u, NULL, 2u, &count));
    TEST_ASSERT_EQUAL(CAN_FLASH_HOST_STATUS_INVALID_ARG, can_flash_host_discover(&s_host, 0u, 1u, found, 2u, NULL));
}
static void test_discover_transport_error(void)
{
    s_fail_send = true;
    can_flash_host_node_t found[1];
    uint32_t count;
    TEST_ASSERT_EQUAL(CAN_FLASH_HOST_STATUS_TRANSPORT, discover(found, 1u, &count));
}
static void test_scan_still_lists_updaters_only(void)
{
    mixed_bus();
    can_flash_host_info_t found[NODES_MAX];
    uint32_t count;
    TEST_ASSERT_EQUAL(CAN_FLASH_HOST_STATUS_OK, can_flash_host_scan(&s_host, CAN_FLASH_HOST_SCAN_FROM,
                      CAN_FLASH_HOST_SCAN_TO, found, NODES_MAX, &count));
    TEST_ASSERT_EQUAL_UINT32(3u, count);
    TEST_ASSERT_EQUAL_UINT32(0u, s_commands[CAN_UPDATE_CMD_APP_INFO]);
}

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
    RUN_TEST(test_app_info_running_app);
    RUN_TEST(test_app_ignores_updater_commands);
    RUN_TEST(test_app_info_updater_nak);
    RUN_TEST(test_app_info_timeout);
    RUN_TEST(test_app_info_malformed);
    RUN_TEST(test_app_info_partial_burst_retried);
    RUN_TEST(test_enter_updater_then_flash);
    RUN_TEST(test_enter_updater_lost_ack);
    RUN_TEST(test_enter_updater_already_parked);
    RUN_TEST(test_enter_updater_refused);
    RUN_TEST(test_enter_updater_timeout);
    RUN_TEST(test_enter_updater_malformed);
    RUN_TEST(test_app_control_invalid_args);
    RUN_TEST(test_discover_mixed_apps_and_updaters);
    RUN_TEST(test_discover_silence_costs_one_window_per_id);
    RUN_TEST(test_discover_ignores_non_standard_and_foreign_frames);
    RUN_TEST(test_discover_skips_corrupt_and_partial_app_bursts);
    RUN_TEST(test_discover_capacity_and_invalid_args);
    RUN_TEST(test_discover_transport_error);
    RUN_TEST(test_scan_still_lists_updaters_only);
    return UNITY_END();
}
