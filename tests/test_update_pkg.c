/**
 * @file    test_update_pkg.c
 * @brief   Host-side Unity tests for the update package v1 manifest parser.
 *
 * A 2 KB byte buffer holds a generated package whose manifest CRC32 is recomputed after each
 * mutation.
 *
 * Covers:       Valid single- and multi-entry packages, an unaligned buffer, bad magic, version
 *               and CRC, truncated manifests and a wrong total size, payload bounds, entry
 *               counts and reserved fields, product names and CAN IDs.
 * Not covered:  Payload SHA-256 checks and staging (brain/main/package_update.c).
 *
 * @see     update_pkg.h, docs/reference/package-format.md, rules/testing.md
 */

#include "unity.h"
#include "update_pkg.h"
#include "crc32.h"
#include <stdio.h>
#include <string.h>

#define TEST_SIZE 2048u
#define PAYLOAD_SIZE 64u
static uint8_t bytes[TEST_SIZE];
static update_pkg_t pkg;
static uint32_t total;

void setUp(void) { memset(bytes, 0, sizeof(bytes)); }
void tearDown(void) { }
static void put16(uint32_t pos, uint16_t value)
{
    bytes[pos] = (uint8_t)value; bytes[pos + 1u] = (uint8_t)(value >> 8);
}
static void put32(uint32_t pos, uint32_t value)
{
    for (uint32_t i = 0; i < 4u; ++i) { bytes[pos + i] = (uint8_t)(value >> (i * 8u)); }
}
static void check(void)
{
    const uint32_t size = UPDATE_PKG_HEADER_SIZE + (uint32_t)bytes[14] * UPDATE_PKG_ENTRY_SIZE;
    put32(24, 0u); put32(24, crc32_ieee(bytes, size));
}
static void valid(uint16_t count)
{
    memset(bytes, 0, sizeof(bytes));
    memcpy(bytes, UPDATE_PKG_MAGIC, UPDATE_PKG_MAGIC_SIZE);
    put16(8, UPDATE_PKG_VERSION); put16(10, UPDATE_PKG_HEADER_SIZE);
    put16(12, UPDATE_PKG_ENTRY_SIZE); put16(14, count);
    const uint32_t ms = UPDATE_PKG_HEADER_SIZE + count * UPDATE_PKG_ENTRY_SIZE;
    total = ms + count * PAYLOAD_SIZE;
    put32(16, ms); put32(20, total);
    for (uint16_t i = 0; i < count; ++i) {
        const uint32_t p = UPDATE_PKG_HEADER_SIZE + i * UPDATE_PKG_ENTRY_SIZE;
        bytes[p] = i == 0 && count > 1 ? UPDATE_PKG_BRAIN : UPDATE_PKG_NODE;
        put32(p + 4, ms + i * PAYLOAD_SIZE); put32(p + 8, PAYLOAD_SIZE);
        if (bytes[p] == UPDATE_PKG_NODE) { put16(p + 12, 0x79E); put16(p + 14, 0x79F); }
    }
    check();
}
static update_pkg_status_t parse(void) { return update_pkg_parse(bytes, sizeof(bytes), total, &pkg); }
static void test_valid_single_multi(void)
{
    valid(1); TEST_ASSERT_EQUAL(UPDATE_PKG_OK, parse()); TEST_ASSERT_EQUAL(1, pkg.count);
    TEST_ASSERT_EQUAL_HEX16(0x79E, pkg.entries[0].cmd_id);
    valid(9); TEST_ASSERT_EQUAL(UPDATE_PKG_OK, parse()); TEST_ASSERT_EQUAL(9, pkg.count);
    valid(1); bytes[32] = UPDATE_PKG_BRAIN; put32(44, 0); check();
    TEST_ASSERT_EQUAL(UPDATE_PKG_OK, parse());
    valid(8); bytes[32] = UPDATE_PKG_NODE; check();
    TEST_ASSERT_EQUAL(UPDATE_PKG_OK, parse());
    /* A bytewise decoder must not depend on aligned host struct accesses. */
    memmove(bytes + 1, bytes, total);
    TEST_ASSERT_EQUAL(UPDATE_PKG_OK, update_pkg_parse(bytes + 1, total, total, &pkg));
}
static void test_magic_version_check(void)
{
    valid(1); bytes[0] ^= 1; TEST_ASSERT_EQUAL(UPDATE_PKG_MAGIC_ERROR, parse());
    valid(1); put16(8, 2); TEST_ASSERT_EQUAL(UPDATE_PKG_VERSION_ERROR, parse());
    valid(1); bytes[24] ^= 1; TEST_ASSERT_EQUAL(UPDATE_PKG_CHECK_ERROR, parse());
    valid(1); bytes[48] ^= 1; TEST_ASSERT_EQUAL(UPDATE_PKG_CHECK_ERROR, parse());
}
static void test_truncated(void)
{
    valid(1);
    TEST_ASSERT_EQUAL(UPDATE_PKG_TRUNCATED, update_pkg_parse(NULL, 0, total, &pkg));
    TEST_ASSERT_EQUAL(UPDATE_PKG_TRUNCATED, update_pkg_parse(bytes, 31, total, &pkg));
    TEST_ASSERT_EQUAL(UPDATE_PKG_TRUNCATED, update_pkg_parse(bytes, 127, total, &pkg));
    TEST_ASSERT_EQUAL(UPDATE_PKG_LAYOUT_ERROR, update_pkg_parse(bytes, sizeof(bytes), total - 1, &pkg));
    /* Payload bytes need not be present in the parser's manifest buffer. */
    TEST_ASSERT_EQUAL(UPDATE_PKG_OK, update_pkg_parse(bytes, 128, total, &pkg));
}
static void test_payload_bounds(void)
{
    valid(1); put32(36, total); check(); TEST_ASSERT_EQUAL(UPDATE_PKG_PAYLOAD_ERROR, parse());
    valid(1); put32(36, UINT32_MAX - 3u); check(); TEST_ASSERT_EQUAL(UPDATE_PKG_PAYLOAD_ERROR, parse());
    valid(1); put32(40, UINT32_MAX); check(); TEST_ASSERT_EQUAL(UPDATE_PKG_PAYLOAD_ERROR, parse());
    valid(1); put32(36, 32); check(); TEST_ASSERT_EQUAL(UPDATE_PKG_PAYLOAD_ERROR, parse());
    valid(1); put32(36, 129); check(); TEST_ASSERT_EQUAL(UPDATE_PKG_PAYLOAD_ERROR, parse());
    valid(1); put32(40, 0); check(); TEST_ASSERT_EQUAL(UPDATE_PKG_PAYLOAD_ERROR, parse());
    valid(2); put32(132, 224); check(); TEST_ASSERT_EQUAL(UPDATE_PKG_PAYLOAD_ERROR, parse());
}
static void test_counts_types_flags(void)
{
    valid(1); put16(14, 0); TEST_ASSERT_EQUAL(UPDATE_PKG_LAYOUT_ERROR, parse());
    valid(1); put16(14, 10); TEST_ASSERT_EQUAL(UPDATE_PKG_LAYOUT_ERROR, parse());
    valid(2); bytes[128] = UPDATE_PKG_BRAIN; put32(140, 0); check(); TEST_ASSERT_EQUAL(UPDATE_PKG_ENTRY_ERROR, parse());
    valid(9); bytes[32] = UPDATE_PKG_NODE; check(); TEST_ASSERT_EQUAL(UPDATE_PKG_ENTRY_ERROR, parse());
    valid(1); bytes[32] = 3; check(); TEST_ASSERT_EQUAL(UPDATE_PKG_ENTRY_ERROR, parse());
    valid(1); bytes[28] = 1; check(); TEST_ASSERT_EQUAL(UPDATE_PKG_LAYOUT_ERROR, parse());
    valid(1); bytes[33] = 1; check(); TEST_ASSERT_EQUAL(UPDATE_PKG_ENTRY_ERROR, parse());
    valid(1); bytes[112] = 1; check(); TEST_ASSERT_EQUAL(UPDATE_PKG_ENTRY_ERROR, parse());
}
static void test_names_ids(void)
{
    valid(1); bytes[34] = 33; check(); TEST_ASSERT_EQUAL(UPDATE_PKG_NAME_ERROR, parse());
    valid(1); bytes[34] = 1; bytes[80] = 0x80; check(); TEST_ASSERT_EQUAL(UPDATE_PKG_NAME_ERROR, parse());
    valid(1); bytes[80] = 'x'; check(); TEST_ASSERT_EQUAL(UPDATE_PKG_NAME_ERROR, parse());
    valid(1); put16(44, 0x800); check(); TEST_ASSERT_EQUAL(UPDATE_PKG_CAN_ID_ERROR, parse());
    valid(1); put16(46, 0x800); check(); TEST_ASSERT_EQUAL(UPDATE_PKG_CAN_ID_ERROR, parse());
    valid(1); bytes[34] = 32; memset(bytes + 80, 'A', 32); check();
    TEST_ASSERT_EQUAL(UPDATE_PKG_OK, parse()); TEST_ASSERT_EQUAL(0, pkg.entries[0].name[32]);
}
static void test_tool_vector(void)
{
    FILE *f = fopen(UPDATE_PKG_VECTOR, "rb");
    TEST_ASSERT_NOT_NULL(f);
    size_t size = fread(bytes, 1, sizeof(bytes), f);
    TEST_ASSERT_EQUAL(0, fclose(f));
    TEST_ASSERT_EQUAL(UPDATE_PKG_OK, update_pkg_parse(bytes, size, (uint32_t)size, &pkg));
    TEST_ASSERT_EQUAL(2, pkg.count);
    TEST_ASSERT_EQUAL_STRING("bench-node", pkg.entries[1].name);
    TEST_ASSERT_EQUAL(UPDATE_PKG_BRAIN, pkg.entries[0].type);
    /* hashlib.sha256 over the deterministic 64-byte sample image. */
    TEST_ASSERT_EQUAL_UINT32(0xE9, bytes[pkg.entries[0].offset]);
    TEST_ASSERT_EQUAL_MEMORY(pkg.entries[0].sha256, pkg.entries[1].sha256, UPDATE_PKG_SHA_SIZE);
}

int main(int argc, char **argv)
{
    /* Also permits cross-checking real tool output without rebuilding tests. */
    if (argc == 2) {
        FILE *f = fopen(argv[1], "rb");
        if (f == NULL || fseek(f, 0, SEEK_END) != 0) { return 2; }
        const long length = ftell(f);
        if (length <= 0 || (unsigned long)length > UINT32_MAX || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return 2; }
        const size_t got = fread(bytes, 1, UPDATE_PKG_MANIFEST_MAX, f);
        if (fclose(f) != 0) { return 2; }
        const update_pkg_status_t result = update_pkg_parse(bytes, got, (uint32_t)length, &pkg);
        puts(update_pkg_status_text(result));
        return result == UPDATE_PKG_OK ? 0 : 1;
    }
    UNITY_BEGIN();
    RUN_TEST(test_valid_single_multi); RUN_TEST(test_magic_version_check);
    RUN_TEST(test_truncated); RUN_TEST(test_payload_bounds); RUN_TEST(test_counts_types_flags);
    RUN_TEST(test_names_ids); RUN_TEST(test_tool_vector);
    return UNITY_END();
}
