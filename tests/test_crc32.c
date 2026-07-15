/* =============================================================================
 * test_crc32.c — Host-side Unity tests for crc32_ieee
 * =============================================================================
 * Golden vectors for the IEEE 802.3 / zlib-compatible CRC32 (reflected,
 * poly 0xEDB88320, init 0xFFFFFFFF, final XOR 0xFFFFFFFF).
 * ============================================================================= */

#include <string.h>

#include "unity.h"

#include "crc32.h"

void setUp(void) {}
void tearDown(void) {}

/* -----------------------------------------------------------------------------
 * Golden vectors
 * ----------------------------------------------------------------------------- */
static void test_check_value_123456789(void)
{
    /* The canonical CRC32 "check" value. */
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926u,
                            crc32_ieee((const uint8_t *)"123456789", 9u));
}

static void test_empty_input_is_zero(void)
{
    /* init ^ final-xor cancel: CRC of zero bytes is 0 (zlib crc32("") == 0). */
    TEST_ASSERT_EQUAL_HEX32(0x00000000u, crc32_ieee((const uint8_t *)"", 0u));
}

static void test_single_byte_a(void)
{
    TEST_ASSERT_EQUAL_HEX32(0xE8B7BE43u, crc32_ieee((const uint8_t *)"a", 1u));
}

static void test_all_0xff_write_unit(void)
{
    /* One erased flash write unit — exercises high-bit bytes. Cross-checked
     * against an independent zlib-compatible CRC32 implementation. */
    static const uint8_t erased[8] = {0xFFu, 0xFFu, 0xFFu, 0xFFu,
                                      0xFFu, 0xFFu, 0xFFu, 0xFFu};
    TEST_ASSERT_EQUAL_HEX32(0x2144DF1Cu, crc32_ieee(erased, sizeof(erased)));
}

static void test_incremental_vs_whole_differs_by_design(void)
{
    /* crc32_ieee is one-shot (no running-state API); a concatenation is NOT
     * the CRC of the parts. Guard against accidental "resumable" misuse. */
    const uint8_t part_a[] = {'1', '2', '3', '4'};
    const uint8_t whole[]  = {'1', '2', '3', '4', '1', '2', '3', '4'};
    TEST_ASSERT_NOT_EQUAL(crc32_ieee(part_a, 4u), crc32_ieee(whole, 8u));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_check_value_123456789);
    RUN_TEST(test_empty_input_is_zero);
    RUN_TEST(test_single_byte_a);
    RUN_TEST(test_all_0xff_write_unit);
    RUN_TEST(test_incremental_vs_whole_differs_by_design);
    return UNITY_END();
}
