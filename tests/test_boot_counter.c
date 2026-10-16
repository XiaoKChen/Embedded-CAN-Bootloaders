#include "boot_counter.h"
#include "unity.h"
#include <string.h>

static uint8_t flash[BOOT_COUNTER_FLASH_BYTES];
void setUp(void) { memset(flash, 0xff, sizeof(flash)); }
void tearDown(void) { }

static void record(size_t slot)
{
    memcpy(flash + slot * BOOT_COUNTER_RECORD_BYTES, boot_counter_record, BOOT_COUNTER_RECORD_BYTES);
}

static void test_rtc_threshold_sticky_clear(void)
{
    boot_counter_rtc_t rtc;
    boot_counter_rtc_clear(&rtc);
    TEST_ASSERT_FALSE(boot_counter_rtc_boot(&rtc, BOOT_COUNTER_POWER_ON));
    TEST_ASSERT_FALSE(boot_counter_rtc_boot(&rtc, BOOT_COUNTER_CRASH));
    TEST_ASSERT_FALSE(boot_counter_rtc_boot(&rtc, BOOT_COUNTER_SOFTWARE));
    TEST_ASSERT_TRUE(boot_counter_rtc_boot(&rtc, BOOT_COUNTER_CRASH));
    boot_counter_rtc_clean_restart(&rtc);
    TEST_ASSERT_TRUE(boot_counter_rtc_boot(&rtc, BOOT_COUNTER_SOFTWARE));
    TEST_ASSERT_TRUE(boot_counter_rtc_boot(&rtc, BOOT_COUNTER_SLEEP));
    TEST_ASSERT_EQUAL_UINT32(BOOT_COUNTER_LIMIT, rtc.count);
    boot_counter_rtc_clear(&rtc);
    TEST_ASSERT_EQUAL_UINT32(0, rtc.count);
    TEST_ASSERT_TRUE(boot_counter_rtc_valid(&rtc));
}

static void test_rtc_clean_mark_is_one_use_and_only_software(void)
{
    boot_counter_rtc_t rtc;
    boot_counter_rtc_clear(&rtc);
    for (unsigned i = 0; i < 5; ++i) {
        boot_counter_rtc_clean_restart(&rtc);
        TEST_ASSERT_FALSE(boot_counter_rtc_boot(&rtc, BOOT_COUNTER_SOFTWARE));
    }
    TEST_ASSERT_EQUAL_UINT32(0, rtc.count);
    TEST_ASSERT_FALSE(boot_counter_rtc_boot(&rtc, BOOT_COUNTER_SOFTWARE));
    TEST_ASSERT_EQUAL_UINT32(1, rtc.count);
    boot_counter_rtc_clean_restart(&rtc);
    TEST_ASSERT_FALSE(boot_counter_rtc_boot(&rtc, BOOT_COUNTER_CRASH));
    TEST_ASSERT_EQUAL_UINT32(2, rtc.count);
}

static void test_rtc_garbage_and_power_on(void)
{
    boot_counter_rtc_t rtc;
    memset(&rtc, 0xff, sizeof(rtc));
    TEST_ASSERT_FALSE(boot_counter_rtc_valid(&rtc));
    TEST_ASSERT_FALSE(boot_counter_rtc_boot(&rtc, BOOT_COUNTER_POWER_ON));
    TEST_ASSERT_EQUAL_UINT32(0, rtc.count);
    rtc.check ^= 1u;
    TEST_ASSERT_FALSE(boot_counter_rtc_valid(&rtc));
    TEST_ASSERT_FALSE(boot_counter_rtc_boot(&rtc, BOOT_COUNTER_SLEEP));
    TEST_ASSERT_EQUAL_UINT32(0, rtc.count);
    rtc.count = UINT32_MAX;
    TEST_ASSERT_FALSE(boot_counter_rtc_valid(&rtc));
    boot_counter_rtc_clean_restart(&rtc);
    TEST_ASSERT_TRUE(boot_counter_rtc_valid(&rtc));
}

static void test_log_threshold_sticky_clear(void)
{
    for (size_t i = 0; i < BOOT_COUNTER_LIMIT; ++i) {
        boot_counter_log_t log = boot_counter_log_scan(flash, sizeof(flash));
        TEST_ASSERT_EQUAL_UINT32(i, log.count);
        TEST_ASSERT_EQUAL_UINT32(i * BOOT_COUNTER_RECORD_BYTES, log.next_slot);
        record(i);
    }
    record(3);
    boot_counter_log_t log = boot_counter_log_scan(flash, sizeof(flash));
    TEST_ASSERT_EQUAL_UINT32(BOOT_COUNTER_LIMIT, log.count);
    TEST_ASSERT_FALSE(boot_counter_log_wrap(&log));
    memset(flash, 0xff, sizeof(flash));
    log = boot_counter_log_scan(flash, sizeof(flash));
    TEST_ASSERT_EQUAL_UINT32(0, log.count);
    TEST_ASSERT_FALSE(log.dirty);
}

static void test_log_torn_record_every_bit(void)
{
    /* Any single weak programmed bit must invalidate the record. */
    const uint8_t *token = (const uint8_t *)boot_counter_record;
    for (size_t byte = 0; byte < BOOT_COUNTER_RECORD_BYTES; ++byte) {
        for (unsigned bit = 0; bit < 8; ++bit) {
            if ((token[byte] & (1u << bit)) == 0u) {
                memset(flash, 0xff, sizeof(flash));
                record(0);
                flash[byte] |= (uint8_t)(1u << bit);
                boot_counter_log_t log = boot_counter_log_scan(flash, sizeof(flash));
                TEST_ASSERT_EQUAL_UINT32(0, log.count);
                TEST_ASSERT_EQUAL_UINT32(BOOT_COUNTER_RECORD_BYTES, log.next_slot);
                record(1); record(2); record(3);
                TEST_ASSERT_EQUAL_UINT32(BOOT_COUNTER_LIMIT,
                    boot_counter_log_scan(flash, sizeof(flash)).count);
            }
        }
    }
}

static void test_log_garbage_full_wrap_and_full_trip(void)
{
    memset(flash, 0xa5, sizeof(flash));
    boot_counter_log_t log = boot_counter_log_scan(flash, sizeof(flash));
    TEST_ASSERT_EQUAL_UINT32(0, log.count);
    TEST_ASSERT_TRUE(boot_counter_log_wrap(&log));
    record(0); record(1);
    log = boot_counter_log_scan(flash, sizeof(flash));
    TEST_ASSERT_EQUAL_UINT32(2, log.count);
    TEST_ASSERT_TRUE(boot_counter_log_wrap(&log));
    /* Adapter erases only on full untripped log, then restores count + boot. */
    const uint32_t target = log.count + 1u;
    memset(flash, 0xff, sizeof(flash));
    for (size_t i = 0; i < target; ++i) { record(i); }
    TEST_ASSERT_EQUAL_UINT32(BOOT_COUNTER_LIMIT, boot_counter_log_scan(flash, sizeof(flash)).count);
    for (size_t i = 0; i < sizeof(flash) / BOOT_COUNTER_RECORD_BYTES; ++i) { record(i); }
    log = boot_counter_log_scan(flash, sizeof(flash));
    TEST_ASSERT_EQUAL_UINT32(BOOT_COUNTER_LIMIT, log.count);
    TEST_ASSERT_FALSE(boot_counter_log_wrap(&log));
}

static void test_log_empty_and_truncated(void)
{
    TEST_ASSERT_EQUAL_UINT32(0, boot_counter_log_scan(NULL, 0).count);
    record(0);
    TEST_ASSERT_EQUAL_UINT32(0, boot_counter_log_scan(flash, BOOT_COUNTER_RECORD_BYTES - 1).count);
    TEST_ASSERT_EQUAL_UINT32(1, boot_counter_log_scan(flash, BOOT_COUNTER_RECORD_BYTES).count);
}

static void test_verified_boot_clear_releases_both_latches(void)
{
    boot_counter_rtc_t rtc;
    boot_counter_rtc_clear(&rtc);
    for (size_t i = 0; i < BOOT_COUNTER_LIMIT; ++i) {
        (void)boot_counter_rtc_boot(&rtc, BOOT_COUNTER_CRASH);
        record(i);
    }
    TEST_ASSERT_EQUAL_UINT32(BOOT_COUNTER_LIMIT, rtc.count);
    TEST_ASSERT_EQUAL_UINT32(BOOT_COUNTER_LIMIT, boot_counter_log_scan(flash, sizeof(flash)).count);
    /* Verified GO uses the same clear primitive without waiting for a timer. */
    memset(flash, 0xff, sizeof(flash));
    boot_counter_rtc_clear(&rtc);
    boot_counter_rtc_clean_restart(&rtc);
    TEST_ASSERT_FALSE(boot_counter_rtc_boot(&rtc, BOOT_COUNTER_SOFTWARE));
    TEST_ASSERT_EQUAL_UINT32(0, rtc.count);
    TEST_ASSERT_EQUAL_UINT32(0, boot_counter_log_scan(flash, sizeof(flash)).count);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_rtc_threshold_sticky_clear);
    RUN_TEST(test_rtc_clean_mark_is_one_use_and_only_software);
    RUN_TEST(test_rtc_garbage_and_power_on);
    RUN_TEST(test_log_threshold_sticky_clear);
    RUN_TEST(test_log_torn_record_every_bit);
    RUN_TEST(test_log_garbage_full_wrap_and_full_trip);
    RUN_TEST(test_log_empty_and_truncated);
    RUN_TEST(test_verified_boot_clear_releases_both_latches);
    return UNITY_END();
}
