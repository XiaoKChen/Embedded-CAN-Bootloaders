/* =============================================================================
 * image_header.h — Application image metadata layout
 * =============================================================================
 * A 16-byte little-endian metadata record lives at METADATA_ADDR (the last
 * 2 KB flash block, see memory_map.h). It is written LAST during an update:
 * power loss at any earlier instant leaves the block erased, the image
 * unvalidated, and the device parked in the bootloader — never bricked.
 *
 * image_crc32 covers flash [APP_BASE, APP_BASE + image_size).
 * header_crc32 covers the first 12 bytes of this record as stored in flash.
 *
 * Update-in-progress marker: 8 bytes at METADATA_ADDR + IMAGE_PENDING_OFFSET,
 * programmed right after the metadata block is erased and BEFORE the first
 * app-slot erase (CAN ERASE and USB DFU block 0). It sits in its own write
 * unit, so the record at offset 0 is later programmed into the same block
 * without a second erase. Marker present + no valid record = an interrupted
 * update: park in the bootloader, never take the stock SP-word fallback
 * (boot_decide in image_validate.h). A torn marker write cannot match both
 * words, and at that instant the app slot is still intact.
 *
 * Pure host-compilable header: <stdint.h> only, no target includes.
 * ============================================================================= */

#ifndef IMAGE_HEADER_H
#define IMAGE_HEADER_H

#include <stdint.h>

#include "boot_config.h"  /* IMAGE_META_MAGIC */
#include "memory_map.h"   /* FLASH_WRITE_UNIT, METADATA_BLOCK_SIZE */

#ifdef __cplusplus
extern "C" {
#endif

/* -----------------------------------------------------------------------------
 * Metadata record — exactly 16 bytes, little-endian in flash
 * ----------------------------------------------------------------------------- */
typedef struct {
    uint32_t magic;         /* IMAGE_META_MAGIC                                 */
    uint32_t image_size;    /* bytes covered by image_crc32, from APP_BASE      */
    uint32_t image_crc32;   /* crc32_ieee over [APP_BASE, APP_BASE+image_size)  */
    uint32_t header_crc32;  /* crc32_ieee over the first 12 bytes of the record */
} image_meta_t;

#define IMAGE_META_SIZE         16u
#define IMAGE_META_CRC_COVERAGE 12u  /* bytes covered by header_crc32 */

_Static_assert(sizeof(image_meta_t) == IMAGE_META_SIZE,
               "image_meta_t must be exactly 16 bytes (4 x uint32_t, no padding)");

/* -----------------------------------------------------------------------------
 * Update-in-progress marker — two little-endian words: magic, ~magic
 * ----------------------------------------------------------------------------- */
#define IMAGE_PENDING_OFFSET 0x10u        /* from METADATA_ADDR               */
#define IMAGE_PENDING_SIZE   8u           /* exactly one code-flash write unit */
#define IMAGE_PENDING_MAGIC  0xB007F1A5u  /* second word is its complement    */

_Static_assert(IMAGE_PENDING_OFFSET >= IMAGE_META_SIZE,
               "pending marker must not overlap the metadata record");
_Static_assert(IMAGE_PENDING_OFFSET % FLASH_WRITE_UNIT == 0u &&
               IMAGE_PENDING_SIZE == FLASH_WRITE_UNIT,
               "pending marker must be exactly one aligned write unit");
_Static_assert(IMAGE_PENDING_OFFSET + IMAGE_PENDING_SIZE <= METADATA_BLOCK_SIZE,
               "pending marker must lie inside the metadata block");

#ifdef __cplusplus
}
#endif

#endif /* IMAGE_HEADER_H */
