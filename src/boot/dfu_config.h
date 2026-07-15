/* =============================================================================
 * dfu_config.h — Pure-logic parse/validate of the flash-time DFU name config
 * =============================================================================
 * At boot the bootloader reads a 76-byte config block from RA4M1 data flash
 * (memory_map.h DFU_CONFIG_ADDR) to override the compiled USB DFU manufacturer
 * (string index 1) and product (string index 2) strings. Absent or invalid
 * block => fall back to the compiled defaults, so unprovisioned boards behave
 * exactly as before.
 *
 * FROZEN SHARED CONTRACT — identical in the provisioning flasher. Do not change
 * any value or offset here without changing it there in lock-step.
 *
 * Block layout (little-endian, 76 bytes):
 *   0x00 u32  magic    = DFU_CONFIG_MAGIC
 *   0x04 u8   version  = DFU_CONFIG_VERSION
 *   0x05 u8   mfr_len  (0..32)
 *   0x06 u8   prod_len (0..32)
 *   0x07 u8   reserved = 0xFF
 *   0x08 char[32] manufacturer (ASCII 0x20..0x7E; bytes past mfr_len = 0xFF)
 *   0x28 char[32] product      (ASCII 0x20..0x7E; bytes past prod_len = 0xFF)
 *   0x48 u32  crc32    = crc32_ieee over the first 72 bytes (0x00..0x47)
 *
 * Validity (ALL must hold, else fall back): magic matches AND version == 1 AND
 * mfr_len <= 32 AND prod_len <= 32 AND crc32 matches.
 *
 * Pure C over a byte buffer — no flash or peripheral access — so it compiles
 * and unit-tests on the host (tests/), mirroring image_validate.{h,c}.
 * ============================================================================= */

#ifndef DFU_CONFIG_H
#define DFU_CONFIG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -----------------------------------------------------------------------------
 * Frozen contract constants
 * ----------------------------------------------------------------------------- */
#define DFU_CONFIG_MAGIC        0xDF0C0FDFu
#define DFU_CONFIG_VERSION      1u
#define DFU_CONFIG_STR_MAX      32u          /* max ASCII chars per string       */
#define DFU_CONFIG_SIZE         76u          /* total block size                 */
#define DFU_CONFIG_CRC_COVERAGE 72u          /* crc32 over bytes 0x00..0x47      */

/* Fixed byte offsets into the block (aliasing-safe accessors, mirrors
 * image_validate.c: explicit little-endian byte loads, never struct overlay). */
#define DFU_CONFIG_OFF_MAGIC    0x00u
#define DFU_CONFIG_OFF_VERSION  0x04u
#define DFU_CONFIG_OFF_MFR_LEN  0x05u
#define DFU_CONFIG_OFF_PROD_LEN 0x06u
#define DFU_CONFIG_OFF_RESERVED 0x07u
#define DFU_CONFIG_OFF_MFR      0x08u
#define DFU_CONFIG_OFF_PROD     0x28u
#define DFU_CONFIG_OFF_CRC      0x48u

/* -----------------------------------------------------------------------------
 * Result codes — ordered to match the check order; frozen contract
 * ----------------------------------------------------------------------------- */
typedef enum {
    DFU_CFG_OK = 0,
    DFU_CFG_ERR_MAGIC,   /* magic missing (e.g. erased data-flash block)         */
    DFU_CFG_ERR_VERSION, /* version != DFU_CONFIG_VERSION                         */
    DFU_CFG_ERR_LEN,     /* mfr_len or prod_len > DFU_CONFIG_STR_MAX             */
    DFU_CFG_ERR_CRC,     /* crc32 over the first 72 bytes mismatch                */
} dfu_config_status_t;

/* -----------------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------------- */

/**
 * Parse and validate a 76-byte config image.
 *
 * On DFU_CFG_OK, *out_mfr / *out_prod point into @p blob at the manufacturer /
 * product byte arrays and *out_*_len give the ASCII length (<= 32). On any
 * failure the out-params are left untouched and the caller must fall back to
 * the compiled defaults. Pure: no hardware access.
 *
 * @param blob         76-byte config block (on target: DFU_CONFIG_ADDR).
 * @param out_mfr      Receives pointer to the manufacturer bytes within @p blob.
 * @param out_mfr_len  Receives the manufacturer ASCII length (<= 32).
 * @param out_prod     Receives pointer to the product bytes within @p blob.
 * @param out_prod_len Receives the product ASCII length (<= 32).
 */
dfu_config_status_t dfu_config_parse(const uint8_t *blob,
                                     const char **out_mfr, uint8_t *out_mfr_len,
                                     const char **out_prod, uint8_t *out_prod_len);

#ifdef __cplusplus
}
#endif

#endif /* DFU_CONFIG_H */
