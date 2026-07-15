/* =============================================================================
 * crc32.h — CRC32 (IEEE 802.3 / zlib-compatible)
 * =============================================================================
 * Reflected, polynomial 0xEDB88320, init 0xFFFFFFFF, final XOR 0xFFFFFFFF.
 * Table-less bitwise implementation: ~100 bytes of code, no 1 KB table —
 * the bootloader flash budget matters more than CRC throughput.
 *
 * Golden vector: crc32_ieee("123456789", 9) == 0xCBF43926.
 *
 * Pure C over a byte buffer — host-testable, no hardware access.
 * ============================================================================= */

#ifndef CRC32_H
#define CRC32_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint32_t crc32_ieee(const uint8_t *data, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif /* CRC32_H */
