/**
 * @file    test_ota_select.c
 * @brief   Host-side Unity tests for the bootloader-equivalent otadata slot selection.
 *
 * Covers:       VALID only, NEW, PENDING_VERIFY with and without an older VALID entry, both
 *               erased, bad CRC, INVALID/ABORTED entries, undefined state, the larger sequence
 *               winning, sequence 0 and 0xFFFFFFFE, app_count 0 and 3, and the CRC golden value.
 * Not covered:  Reading otadata from flash and esp_image_verify (node_updater/main/main.c).
 *
 * @see     ota_select.h
 */

#include <string.h>

#include "unity.h"

#include "ota_select.h"

void setUp(void) {}
void tearDown(void) {}

static ota_select_entry_t entry(uint32_t seq, uint32_t state)
{
    ota_select_entry_t e;
    memset(&e, 0xFF, sizeof(e));
    e.ota_seq   = seq;
    e.ota_state = state;
    e.crc       = ota_select_crc(seq);
    return e;
}

static ota_select_entry_t erased(void)
{
    ota_select_entry_t e;
    memset(&e, 0xFF, sizeof(e));
    return e;
}

static int pick(ota_select_entry_t a, ota_select_entry_t b, uint32_t app_count)
{
    const ota_select_entry_t pair[2] = { a, b };
    return ota_select_boot_slot(pair, app_count);
}

static void test_layout_and_crc_golden(void)
{
    TEST_ASSERT_EQUAL_UINT32(32u, sizeof(ota_select_entry_t));
    TEST_ASSERT_EQUAL_HEX32(0x4743989Au, ota_select_crc(1u));  /* the stock seq-1 otadata CRC */
}

static void test_valid_only(void)
{
    TEST_ASSERT_EQUAL_INT(0, pick(entry(1u, OTA_SELECT_STATE_VALID), erased(), 2u));
    TEST_ASSERT_EQUAL_INT(1, pick(erased(), entry(2u, OTA_SELECT_STATE_VALID), 2u));
    TEST_ASSERT_EQUAL_INT(1, pick(entry(2u, OTA_SELECT_STATE_UNDEFINED), erased(), 2u));
}

static void test_new_is_selectable(void)
{
    TEST_ASSERT_EQUAL_INT(1, pick(entry(2u, OTA_SELECT_STATE_NEW), erased(), 2u));
    TEST_ASSERT_EQUAL_INT(1, pick(entry(1u, OTA_SELECT_STATE_VALID), entry(2u, OTA_SELECT_STATE_NEW), 2u));
}

static void test_pending_with_older_valid_rolls_back_to_the_valid_one(void)
{
    TEST_ASSERT_EQUAL_INT(0, pick(entry(1u, OTA_SELECT_STATE_VALID), entry(2u, OTA_SELECT_STATE_PENDING), 2u));
    TEST_ASSERT_EQUAL_INT(0, pick(entry(2u, OTA_SELECT_STATE_PENDING), entry(1u, OTA_SELECT_STATE_VALID), 2u));
}

static void test_pending_alone_is_not_bootable(void)
{
    TEST_ASSERT_EQUAL_INT(-1, pick(entry(2u, OTA_SELECT_STATE_PENDING), erased(), 2u));
    TEST_ASSERT_EQUAL_INT(-1, pick(erased(), entry(1u, OTA_SELECT_STATE_PENDING), 2u));
}

static void test_both_erased(void)
{
    TEST_ASSERT_EQUAL_INT(-1, pick(erased(), erased(), 2u));
}

static void test_bad_crc_is_discarded(void)
{
    ota_select_entry_t bad = entry(2u, OTA_SELECT_STATE_VALID);
    bad.crc ^= 1u;
    TEST_ASSERT_EQUAL_INT(-1, pick(bad, erased(), 2u));
    TEST_ASSERT_EQUAL_INT(0, pick(entry(1u, OTA_SELECT_STATE_VALID), bad, 2u));
}

static void test_invalid_and_aborted_are_discarded(void)
{
    TEST_ASSERT_EQUAL_INT(-1, pick(entry(1u, OTA_SELECT_STATE_INVALID), erased(), 2u));
    TEST_ASSERT_EQUAL_INT(-1, pick(entry(1u, OTA_SELECT_STATE_ABORTED), entry(2u, OTA_SELECT_STATE_INVALID), 2u));
    TEST_ASSERT_EQUAL_INT(0, pick(entry(1u, OTA_SELECT_STATE_VALID), entry(2u, OTA_SELECT_STATE_ABORTED), 2u));
}

static void test_larger_sequence_wins(void)
{
    TEST_ASSERT_EQUAL_INT(0, pick(entry(3u, OTA_SELECT_STATE_VALID), entry(2u, OTA_SELECT_STATE_VALID), 2u));
    TEST_ASSERT_EQUAL_INT(2, pick(entry(2u, OTA_SELECT_STATE_VALID), entry(3u, OTA_SELECT_STATE_VALID), 3u));
    TEST_ASSERT_EQUAL_INT(1, pick(entry(2u, OTA_SELECT_STATE_VALID), erased(), 3u));
}

static void test_sequence_wrap_follows_unsigned_arithmetic_like_the_bootloader(void)
{
    /* (0 - 1) % 2 == 1 and (0xFFFFFFFE - 1) % 2 == 1 in uint32 arithmetic. */
    TEST_ASSERT_EQUAL_INT(1, pick(entry(0u, OTA_SELECT_STATE_VALID), erased(), 2u));
    TEST_ASSERT_EQUAL_INT(1, pick(entry(0xFFFFFFFEu, OTA_SELECT_STATE_VALID), erased(), 2u));
    /* No wrap-aware ordering: the numerically larger sequence wins. */
    TEST_ASSERT_EQUAL_INT(1, pick(entry(0u, OTA_SELECT_STATE_VALID), entry(0xFFFFFFFEu, OTA_SELECT_STATE_VALID), 2u));
}

static void test_no_app_partitions_means_no_slot(void)
{
    TEST_ASSERT_EQUAL_INT(-1, pick(entry(1u, OTA_SELECT_STATE_VALID), erased(), 0u));
}

static void test_entries_are_not_modified(void)
{
    const ota_select_entry_t pair[2] = { entry(2u, OTA_SELECT_STATE_PENDING), entry(1u, OTA_SELECT_STATE_VALID) };
    (void)ota_select_boot_slot(pair, 2u);
    TEST_ASSERT_EQUAL_UINT32(OTA_SELECT_STATE_PENDING, pair[0].ota_state);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_layout_and_crc_golden);
    RUN_TEST(test_valid_only);
    RUN_TEST(test_new_is_selectable);
    RUN_TEST(test_pending_with_older_valid_rolls_back_to_the_valid_one);
    RUN_TEST(test_pending_alone_is_not_bootable);
    RUN_TEST(test_both_erased);
    RUN_TEST(test_bad_crc_is_discarded);
    RUN_TEST(test_invalid_and_aborted_are_discarded);
    RUN_TEST(test_larger_sequence_wins);
    RUN_TEST(test_sequence_wrap_follows_unsigned_arithmetic_like_the_bootloader);
    RUN_TEST(test_no_app_partitions_means_no_slot);
    RUN_TEST(test_entries_are_not_modified);
    return UNITY_END();
}
