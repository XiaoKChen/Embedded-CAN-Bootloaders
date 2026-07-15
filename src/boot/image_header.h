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
 * Pure host-compilable header: <stdint.h> only, no target includes.
 * ============================================================================= */

#ifndef IMAGE_HEADER_H
#define IMAGE_HEADER_H

#include <stdint.h>

#include "boot_config.h"  /* IMAGE_META_MAGIC */

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

#ifdef __cplusplus
}
#endif

#endif /* IMAGE_HEADER_H */
