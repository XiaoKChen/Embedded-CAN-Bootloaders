/**
 * @file    test_slcan_codec.c
 * @brief   Host-side Unity tests for the SLCAN (Lawicel) codec.
 *
 * Exercises the codec at its seam: CR-stripped command lines in, encoded reply/frame lines out.
 * Pure logic: no hardware, mirrors test_image_validate.c.
 *
 * Covers:       python-can's exact open sequence, every S0..S8 bitrate, standard/extended/RTR
 *               frames at DLC 0..8, the malformed lines the bridge must NAK, and encode -> parse
 *               round-trips.
 * Not covered:  The USB and TWAI I/O of the bridge (examples/slcan_bridge).
 *
 * @see     slcan_codec.h, rules/testing.md
 */

#include <string.h>

#include "unity.h"

#include "slcan_codec.h"


/* -----------------------------------------------------------------------------
 * Fixture
 * ----------------------------------------------------------------------------- */
static slcan_command_t s_cmd;
static char            s_out[SLCAN_FRAME_LINE_MAX];
static size_t          s_out_len;

static slcan_status_t parse(const char *line)
{
    return slcan_parse_line(line, strlen(line), &s_cmd);
}

static void assert_nak(const char *line)
{
    TEST_ASSERT_EQUAL_MESSAGE(SLCAN_STATUS_ERROR, parse(line), line);
}

static void assert_encoded(const char *expected)
{
    TEST_ASSERT_EQUAL_size_t(strlen(expected), s_out_len);
    TEST_ASSERT_EQUAL_MEMORY(expected, s_out, s_out_len);
}

static slcan_frame_t make_frame(uint32_t id, uint8_t dlc, bool extended, bool remote)
{
    slcan_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.id       = id;
    frame.dlc      = dlc;
    frame.extended = extended;
    frame.remote   = remote;
    for (uint8_t i = 0u; i < SLCAN_MAX_DLC; i++) {
        frame.data[i] = (uint8_t)(0xA0u + i);
    }
    return frame;
}

void setUp(void)
{
    memset(&s_cmd, 0xEE, sizeof(s_cmd));  /* parse must overwrite every field */
    memset(s_out, 0, sizeof(s_out));
    s_out_len = 0u;
}

void tearDown(void) {}


/* -----------------------------------------------------------------------------
 * Control commands
 * ----------------------------------------------------------------------------- */
static void test_python_can_open_sequence_parses(void)
{
    /* slcanBus(bitrate=250000): "C\r", "S5\r", "\r" (empty Y code), "O\r",
     * then "O\r" again from __init__ (python-can 4.6.1). */
    TEST_ASSERT_EQUAL(SLCAN_STATUS_OK, parse("C"));
    TEST_ASSERT_EQUAL(SLCAN_CMD_CLOSE, s_cmd.type);

    TEST_ASSERT_EQUAL(SLCAN_STATUS_OK, parse("S5"));
    TEST_ASSERT_EQUAL(SLCAN_CMD_SET_BITRATE, s_cmd.type);
    TEST_ASSERT_EQUAL_UINT32(250000u, s_cmd.bitrate_bps);

    TEST_ASSERT_EQUAL(SLCAN_STATUS_OK, parse(""));
    TEST_ASSERT_EQUAL(SLCAN_CMD_EMPTY, s_cmd.type);

    TEST_ASSERT_EQUAL(SLCAN_STATUS_OK, parse("O"));
    TEST_ASSERT_EQUAL(SLCAN_CMD_OPEN, s_cmd.type);
}

static void test_bare_commands_parse(void)
{
    TEST_ASSERT_EQUAL(SLCAN_STATUS_OK, parse("L"));
    TEST_ASSERT_EQUAL(SLCAN_CMD_LISTEN, s_cmd.type);
    TEST_ASSERT_EQUAL(SLCAN_STATUS_OK, parse("V"));
    TEST_ASSERT_EQUAL(SLCAN_CMD_VERSION, s_cmd.type);
    TEST_ASSERT_EQUAL(SLCAN_STATUS_OK, parse("N"));
    TEST_ASSERT_EQUAL(SLCAN_CMD_SERIAL, s_cmd.type);
}

static void test_every_lawicel_bitrate_parses(void)
{
    static const uint32_t expected_bps[] = {
        10000u, 20000u, 50000u, 100000u, 125000u, 250000u, 500000u, 750000u, 1000000u,
    };
    char line[3] = { 'S', '0', '\0' };
    for (uint8_t i = 0u; i < 9u; i++) {
        line[1] = (char)('0' + i);
        TEST_ASSERT_EQUAL_MESSAGE(SLCAN_STATUS_OK, parse(line), line);
        TEST_ASSERT_EQUAL(SLCAN_CMD_SET_BITRATE, s_cmd.type);
        TEST_ASSERT_EQUAL_UINT32(expected_bps[i], s_cmd.bitrate_bps);
    }
}


/* -----------------------------------------------------------------------------
 * Frame parsing
 * ----------------------------------------------------------------------------- */
static void test_standard_frame_parses(void)
{
    TEST_ASSERT_EQUAL(SLCAN_STATUS_OK, parse("t79E80300AABBCCDDEEFF"));
    TEST_ASSERT_EQUAL(SLCAN_CMD_TRANSMIT, s_cmd.type);
    TEST_ASSERT_EQUAL_UINT32(0x79Eu, s_cmd.frame.id);
    TEST_ASSERT_EQUAL_UINT8(8u, s_cmd.frame.dlc);
    TEST_ASSERT_FALSE(s_cmd.frame.extended);
    TEST_ASSERT_FALSE(s_cmd.frame.remote);
    static const uint8_t expected[8] = { 0x03, 0x00, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF };
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, s_cmd.frame.data, 8u);
}

static void test_extended_frame_parses_lower_case_hex(void)
{
    TEST_ASSERT_EQUAL(SLCAN_STATUS_OK, parse("T1abcdef02beef"));
    TEST_ASSERT_EQUAL(SLCAN_CMD_TRANSMIT, s_cmd.type);
    TEST_ASSERT_EQUAL_UINT32(0x1ABCDEF0u, s_cmd.frame.id);
    TEST_ASSERT_EQUAL_UINT8(2u, s_cmd.frame.dlc);
    TEST_ASSERT_TRUE(s_cmd.frame.extended);
    TEST_ASSERT_FALSE(s_cmd.frame.remote);
    TEST_ASSERT_EQUAL_UINT8(0xBEu, s_cmd.frame.data[0]);
    TEST_ASSERT_EQUAL_UINT8(0xEFu, s_cmd.frame.data[1]);
}

static void test_remote_frames_parse_without_data(void)
{
    TEST_ASSERT_EQUAL(SLCAN_STATUS_OK, parse("r7FF8"));
    TEST_ASSERT_EQUAL(SLCAN_CMD_TRANSMIT, s_cmd.type);
    TEST_ASSERT_EQUAL_UINT32(0x7FFu, s_cmd.frame.id);
    TEST_ASSERT_EQUAL_UINT8(8u, s_cmd.frame.dlc);
    TEST_ASSERT_TRUE(s_cmd.frame.remote);
    TEST_ASSERT_FALSE(s_cmd.frame.extended);

    TEST_ASSERT_EQUAL(SLCAN_STATUS_OK, parse("R1FFFFFFF0"));
    TEST_ASSERT_EQUAL_UINT32(0x1FFFFFFFu, s_cmd.frame.id);
    TEST_ASSERT_EQUAL_UINT8(0u, s_cmd.frame.dlc);
    TEST_ASSERT_TRUE(s_cmd.frame.remote);
    TEST_ASSERT_TRUE(s_cmd.frame.extended);
}

static void test_zero_length_data_frame_parses(void)
{
    TEST_ASSERT_EQUAL(SLCAN_STATUS_OK, parse("t0000"));
    TEST_ASSERT_EQUAL_UINT32(0u, s_cmd.frame.id);
    TEST_ASSERT_EQUAL_UINT8(0u, s_cmd.frame.dlc);
    TEST_ASSERT_FALSE(s_cmd.frame.remote);
}


/* -----------------------------------------------------------------------------
 * Malformed / unsupported lines -> NAK
 * ----------------------------------------------------------------------------- */
static void test_malformed_control_lines_nak(void)
{
    assert_nak("O1");          /* trailing byte                 */
    assert_nak("CC");
    assert_nak("S");           /* missing digit                 */
    assert_nak("S9");          /* python-can's 83.3k, not Lawicel */
    assert_nak("S55");
    assert_nak("Sx");
    assert_nak("s0014");       /* BTR register timing           */
    assert_nak("Y2");          /* CAN FD data bitrate           */
    assert_nak("v");           /* never sent by python-can      */
    assert_nak("F");
    assert_nak("Z1");
    assert_nak("X");
}

static void test_malformed_frames_nak(void)
{
    assert_nak("t");           /* no ID                         */
    assert_nak("t12");         /* short ID                      */
    assert_nak("t123");        /* no DLC                        */
    assert_nak("t800");        /* 11-bit ID out of range ...    */
    assert_nak("t8000");
    assert_nak("T200000000");  /* ... and 29-bit                */
    assert_nak("t12G0");       /* bad hex in ID                 */
    assert_nak("t1239");       /* DLC above 8                   */
    assert_nak("t123A");       /* DLC not a decimal digit       */
    assert_nak("t1232AA");     /* data shorter than DLC         */
    assert_nak("t1231AABB");   /* data longer than DLC          */
    assert_nak("t1231AG");     /* bad hex in data               */
    assert_nak("r1231AA");     /* RTR with data                 */
    assert_nak("d1230");       /* CAN FD                        */
    assert_nak("B123456780");
}

static void test_failed_parse_leaves_no_partial_command(void)
{
    TEST_ASSERT_EQUAL(SLCAN_STATUS_ERROR, parse("t1232AA"));
    TEST_ASSERT_EQUAL(SLCAN_CMD_EMPTY, s_cmd.type);
    TEST_ASSERT_EQUAL_UINT32(0u, s_cmd.frame.id);
    TEST_ASSERT_EQUAL_UINT8(0u, s_cmd.frame.data[0]);
}

static void test_null_arguments_are_rejected(void)
{
    TEST_ASSERT_EQUAL(SLCAN_STATUS_INVALID_ARG, slcan_parse_line("O", 1u, NULL));
    TEST_ASSERT_EQUAL(SLCAN_STATUS_INVALID_ARG, slcan_parse_line(NULL, 1u, &s_cmd));
    TEST_ASSERT_EQUAL(SLCAN_STATUS_OK, slcan_parse_line(NULL, 0u, &s_cmd));

    const slcan_frame_t frame = make_frame(0x123u, 1u, false, false);
    TEST_ASSERT_EQUAL(SLCAN_STATUS_INVALID_ARG, slcan_encode_frame(NULL, s_out, sizeof(s_out), &s_out_len));
    TEST_ASSERT_EQUAL(SLCAN_STATUS_INVALID_ARG, slcan_encode_frame(&frame, NULL, sizeof(s_out), &s_out_len));
    TEST_ASSERT_EQUAL(SLCAN_STATUS_INVALID_ARG, slcan_encode_frame(&frame, s_out, sizeof(s_out), NULL));
}


/* -----------------------------------------------------------------------------
 * Encoding
 * ----------------------------------------------------------------------------- */
static void test_standard_frame_encodes(void)
{
    const slcan_frame_t frame = make_frame(0x79Fu, 8u, false, false);
    TEST_ASSERT_EQUAL(SLCAN_STATUS_OK, slcan_encode_frame(&frame, s_out, sizeof(s_out), &s_out_len));
    assert_encoded("t79F8A0A1A2A3A4A5A6A7\r");
}

static void test_extended_frame_encodes_at_max_length(void)
{
    const slcan_frame_t frame = make_frame(0x1ABCDEF0u, 8u, true, false);
    TEST_ASSERT_EQUAL(SLCAN_STATUS_OK, slcan_encode_frame(&frame, s_out, sizeof(s_out), &s_out_len));
    assert_encoded("T1ABCDEF08A0A1A2A3A4A5A6A7\r");
    TEST_ASSERT_EQUAL_size_t(SLCAN_FRAME_LINE_MAX, s_out_len);
}

static void test_remote_frames_encode_without_data(void)
{
    slcan_frame_t frame = make_frame(0x012u, 3u, false, true);
    TEST_ASSERT_EQUAL(SLCAN_STATUS_OK, slcan_encode_frame(&frame, s_out, sizeof(s_out), &s_out_len));
    assert_encoded("r0123\r");

    frame = make_frame(0x00000001u, 8u, true, true);
    TEST_ASSERT_EQUAL(SLCAN_STATUS_OK, slcan_encode_frame(&frame, s_out, sizeof(s_out), &s_out_len));
    assert_encoded("R000000018\r");
}

static void test_wire_dlc_above_8_encodes_as_8(void)
{
    const slcan_frame_t frame = make_frame(0x100u, 15u, false, false);
    TEST_ASSERT_EQUAL(SLCAN_STATUS_OK, slcan_encode_frame(&frame, s_out, sizeof(s_out), &s_out_len));
    assert_encoded("t1008A0A1A2A3A4A5A6A7\r");
}

static void test_encode_rejects_bad_id_and_short_buffer(void)
{
    slcan_frame_t frame = make_frame(0x800u, 0u, false, false);
    TEST_ASSERT_EQUAL(SLCAN_STATUS_INVALID_ARG, slcan_encode_frame(&frame, s_out, sizeof(s_out), &s_out_len));
    frame = make_frame(0x20000000u, 0u, true, false);
    TEST_ASSERT_EQUAL(SLCAN_STATUS_INVALID_ARG, slcan_encode_frame(&frame, s_out, sizeof(s_out), &s_out_len));

    frame = make_frame(0x123u, 8u, false, false);  /* needs 22 bytes */
    TEST_ASSERT_EQUAL(SLCAN_STATUS_INVALID_ARG, slcan_encode_frame(&frame, s_out, 21u, &s_out_len));
    TEST_ASSERT_EQUAL(SLCAN_STATUS_OK, slcan_encode_frame(&frame, s_out, 22u, &s_out_len));
}

static void test_version_and_serial_replies_encode(void)
{
    TEST_ASSERT_EQUAL(SLCAN_STATUS_OK, slcan_encode_version(1u, 23u, s_out, sizeof(s_out), &s_out_len));
    assert_encoded("V0123\r");
    TEST_ASSERT_EQUAL(SLCAN_STATUS_INVALID_ARG, slcan_encode_version(100u, 0u, s_out, sizeof(s_out), &s_out_len));
    TEST_ASSERT_EQUAL(SLCAN_STATUS_INVALID_ARG, slcan_encode_version(1u, 1u, s_out, 5u, &s_out_len));

    TEST_ASSERT_EQUAL(SLCAN_STATUS_OK, slcan_encode_serial(0xBEEFu, s_out, sizeof(s_out), &s_out_len));
    assert_encoded("NBEEF\r");
    TEST_ASSERT_EQUAL(SLCAN_STATUS_INVALID_ARG, slcan_encode_serial(0u, s_out, 5u, &s_out_len));
}


/* -----------------------------------------------------------------------------
 * Round-trips: what the bridge encodes, a host parses back unchanged
 * ----------------------------------------------------------------------------- */
static void round_trip(const slcan_frame_t *frame)
{
    TEST_ASSERT_EQUAL(SLCAN_STATUS_OK, slcan_encode_frame(frame, s_out, sizeof(s_out), &s_out_len));
    TEST_ASSERT_EQUAL_CHAR('\r', s_out[s_out_len - 1u]);
    TEST_ASSERT_EQUAL(SLCAN_STATUS_OK, slcan_parse_line(s_out, s_out_len - 1u, &s_cmd));

    TEST_ASSERT_EQUAL(SLCAN_CMD_TRANSMIT, s_cmd.type);
    TEST_ASSERT_EQUAL_UINT32(frame->id, s_cmd.frame.id);
    TEST_ASSERT_EQUAL_UINT8(frame->dlc, s_cmd.frame.dlc);
    TEST_ASSERT_EQUAL(frame->extended, s_cmd.frame.extended);
    TEST_ASSERT_EQUAL(frame->remote, s_cmd.frame.remote);
    if (!frame->remote && frame->dlc > 0u) {
        TEST_ASSERT_EQUAL_UINT8_ARRAY(frame->data, s_cmd.frame.data, frame->dlc);
    }
}

static void test_every_dlc_and_frame_kind_round_trips(void)
{
    for (uint8_t dlc = 0u; dlc <= SLCAN_MAX_DLC; dlc++) {
        slcan_frame_t frame = make_frame(SLCAN_STD_ID_MAX, dlc, false, false);
        round_trip(&frame);
        frame = make_frame(SLCAN_EXT_ID_MAX, dlc, true, false);
        round_trip(&frame);
        frame = make_frame(0x000u, dlc, false, true);
        round_trip(&frame);
        frame = make_frame(0x00000000u, dlc, true, true);
        round_trip(&frame);
    }
}

static void test_parsed_upper_case_line_re_encodes_identically(void)
{
    static const char *const lines[] = {
        "t79E80300AABBCCDDEEFF", "t0000", "T1FFFFFFF401020304", "r7FF8", "R000000000",
    };
    for (size_t i = 0u; i < sizeof(lines) / sizeof(lines[0]); i++) {
        TEST_ASSERT_EQUAL_MESSAGE(SLCAN_STATUS_OK, parse(lines[i]), lines[i]);
        const slcan_frame_t frame = s_cmd.frame;
        TEST_ASSERT_EQUAL(SLCAN_STATUS_OK, slcan_encode_frame(&frame, s_out, sizeof(s_out), &s_out_len));
        TEST_ASSERT_EQUAL_size_t(strlen(lines[i]) + 1u, s_out_len);
        TEST_ASSERT_EQUAL_MEMORY(lines[i], s_out, strlen(lines[i]));
    }
}


int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_python_can_open_sequence_parses);
    RUN_TEST(test_bare_commands_parse);
    RUN_TEST(test_every_lawicel_bitrate_parses);
    RUN_TEST(test_standard_frame_parses);
    RUN_TEST(test_extended_frame_parses_lower_case_hex);
    RUN_TEST(test_remote_frames_parse_without_data);
    RUN_TEST(test_zero_length_data_frame_parses);
    RUN_TEST(test_malformed_control_lines_nak);
    RUN_TEST(test_malformed_frames_nak);
    RUN_TEST(test_failed_parse_leaves_no_partial_command);
    RUN_TEST(test_null_arguments_are_rejected);
    RUN_TEST(test_standard_frame_encodes);
    RUN_TEST(test_extended_frame_encodes_at_max_length);
    RUN_TEST(test_remote_frames_encode_without_data);
    RUN_TEST(test_wire_dlc_above_8_encodes_as_8);
    RUN_TEST(test_encode_rejects_bad_id_and_short_buffer);
    RUN_TEST(test_version_and_serial_replies_encode);
    RUN_TEST(test_every_dlc_and_frame_kind_round_trips);
    RUN_TEST(test_parsed_upper_case_line_re_encodes_identically);
    return UNITY_END();
}
