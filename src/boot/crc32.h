/**
 * @file    crc32.h
 * @brief   CRC32 (IEEE 802.3 / zlib-compatible), one-shot or continued over chunks.
 *
 * Reflected, polynomial 0xEDB88320, init 0xFFFFFFFF, final XOR 0xFFFFFFFF. Table-less bitwise
 * implementation: ~100 bytes of code, no 1 KB table — the bootloader flash budget matters more
 * than CRC throughput.
 *
 * Invariants:   Golden vector: crc32_ieee("123456789", 9) == 0xCBF43926.
 * Depends on:   Nothing: pure C over a byte buffer — host-testable, no hardware access.
 *
 * @see     crc32.c, test_crc32.c
 */

#ifndef CRC32_H
#define CRC32_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint32_t crc32_ieee(const uint8_t *data, uint32_t len);

/**
 * Continue a CRC32 over the next chunk of a longer buffer (zlib crc32()
 * semantics): start with crc = 0, feed each chunk's previous result back in.
 * crc32_ieee_update(crc32_ieee_update(0, a, n), b, m) equals crc32_ieee over
 * the concatenation a||b. Lets callers CRC data they can only read in chunks.
 */
uint32_t crc32_ieee_update(uint32_t crc, const uint8_t *data, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif /* CRC32_H */
