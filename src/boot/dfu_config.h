/* =============================================================================
 * dfu_config.h — Pure-logic parse/validate of the flash-time DFU config block
 * =============================================================================
 * At boot the bootloader reads a config block from RA4M1 data flash
 * (memory_map.h DFU_CONFIG_ADDR) to override two things:
 *   1. the compiled USB DFU manufacturer (string index 1) and product
 *      (string index 2) strings, and
 *   2. (v2 only) the per-device CAN update IDs, so several boards can sit in
 *      bootloader mode on one bus and each be addressed individually.
 * An absent or invalid block => fall back to the compiled defaults, so
 * unprovisioned boards behave exactly as before.
 *
 * FROZEN SHARED CONTRACT — identical in the provisioning flasher(s). Do not
 * change any value or offset here without changing it there in lock-step. Two
 * on-disk versions coexist; the bootloader accepts BOTH on read:
 *
 * v1 (version byte = 1, 76 bytes) — no CAN-ID fields, defaults used for IDs:
 *   0x00 u32  magic    = DFU_CONFIG_MAGIC
 *   0x04 u8   version  = 1
 *   0x05 u8   mfr_len  (0..32)
 *   0x06 u8   prod_len (0..32)
 *   0x07 u8   reserved = 0xFF
 *   0x08 char[32] manufacturer (ASCII; bytes past mfr_len = 0xFF)
 *   0x28 char[32] product      (ASCII; bytes past prod_len = 0xFF)
 *   0x48 u32  crc32    = crc32_ieee over the first 72 bytes (0x00..0x47)
 *
 * v2 (version byte = 2, 80 bytes) — adds the two 11-bit CAN IDs:
 *   0x00..0x47  identical to v1 (magic, version=2, lens, reserved, strings)
 *   0x48 u16  boot_cmd_id   (host -> boot, 11-bit standard ID)
 *   0x4A u16  boot_reply_id (boot -> host, 11-bit standard ID)
 *   0x4C u32  crc32    = crc32_ieee over the first 76 bytes (0x00..0x4B)
 *
 * Validity (ALL must hold, else fall back): magic matches AND version in {1,2}
 * AND mfr_len <= 32 AND prod_len <= 32 AND the version's crc32 matches. A CAN-ID
 * field that is not a valid 11-bit value (0x000..0x7FF; 0xFFFF/out-of-range =
 * unset) falls back to that ID's compiled default individually.
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
#define DFU_CONFIG_VERSION_V1   1u
#define DFU_CONFIG_VERSION_V2   2u
#define DFU_CONFIG_VERSION      DFU_CONFIG_VERSION_V2  /* current writer version */
#define DFU_CONFIG_STR_MAX      32u          /* max ASCII chars per string       */

#define DFU_CONFIG_SIZE_V1         76u        /* v1 total block size              */
#define DFU_CONFIG_SIZE_V2         80u        /* v2 total block size              */
#define DFU_CONFIG_SIZE            DFU_CONFIG_SIZE_V2  /* largest; buffer sizing  */
#define DFU_CONFIG_CRC_COVERAGE_V1 72u        /* crc32 over bytes 0x00..0x47      */
#define DFU_CONFIG_CRC_COVERAGE_V2 76u        /* crc32 over bytes 0x00..0x4B      */

#define DFU_CONFIG_CAN_ID_MAX   0x7FFu        /* max 11-bit standard CAN ID       */

/* Fixed byte offsets into the block (aliasing-safe accessors, mirrors
 * image_validate.c: explicit little-endian byte loads, never struct overlay). */
#define DFU_CONFIG_OFF_MAGIC         0x00u
#define DFU_CONFIG_OFF_VERSION       0x04u
#define DFU_CONFIG_OFF_MFR_LEN       0x05u
#define DFU_CONFIG_OFF_PROD_LEN      0x06u
#define DFU_CONFIG_OFF_RESERVED      0x07u
#define DFU_CONFIG_OFF_MFR           0x08u
#define DFU_CONFIG_OFF_PROD          0x28u
#define DFU_CONFIG_OFF_CRC_V1        0x48u    /* v1 trailer                       */
#define DFU_CONFIG_OFF_BOOT_CMD_ID   0x48u    /* v2 u16 host -> boot              */
#define DFU_CONFIG_OFF_BOOT_REPLY_ID 0x4Au    /* v2 u16 boot -> host              */
#define DFU_CONFIG_OFF_CRC_V2        0x4Cu    /* v2 trailer                       */

/* -----------------------------------------------------------------------------
 * Result codes — ordered to match the check order; frozen contract
 * ----------------------------------------------------------------------------- */
typedef enum {
    DFU_CFG_OK = 0,
    DFU_CFG_ERR_MAGIC,   /* magic missing (e.g. erased data-flash block)         */
    DFU_CFG_ERR_VERSION, /* version not in {1, 2}                                 */
    DFU_CFG_ERR_LEN,     /* mfr_len or prod_len > DFU_CONFIG_STR_MAX             */
    DFU_CFG_ERR_CRC,     /* version's crc32 coverage mismatch                     */
} dfu_config_status_t;

/* -----------------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------------- */

/**
 * Parse and validate a config block, extracting the DFU name strings.
 *
 * Accepts both v1 (76-byte) and v2 (80-byte) blocks — the name fields are at
 * the same offsets in both, so a v2-provisioned board keeps its custom USB
 * name. On DFU_CFG_OK, *out_mfr / *out_prod point into @p blob at the
 * manufacturer / product byte arrays and *out_*_len give the ASCII length
 * (<= 32). On any failure the out-params are left untouched and the caller must
 * fall back to the compiled defaults. Pure: no hardware access.
 *
 * @param blob         Config block (on target: DFU_CONFIG_ADDR).
 * @param out_mfr      Receives pointer to the manufacturer bytes within @p blob.
 * @param out_mfr_len  Receives the manufacturer ASCII length (<= 32).
 * @param out_prod     Receives pointer to the product bytes within @p blob.
 * @param out_prod_len Receives the product ASCII length (<= 32).
 */
dfu_config_status_t dfu_config_parse(const uint8_t *blob,
                                     const char **out_mfr, uint8_t *out_mfr_len,
                                     const char **out_prod, uint8_t *out_prod_len);

/**
 * Extract the per-device CAN update IDs from a config block.
 *
 * ALWAYS writes usable IDs. A valid v2 block supplies device-specific IDs, each
 * validated as an 11-bit standard ID (0x000..0x7FF); an absent/invalid block, a
 * v1 block, or an out-of-range ID field yields the compiled defaults
 * (CAN_ID_HOST_TO_BOOT / CAN_ID_BOOT_TO_HOST from boot_config.h), per ID
 * independently. Pure: no hardware access. Split from dfu_config_parse so the
 * USB-descriptor name path keeps its existing signature.
 *
 * @param blob              Config block (on target: DFU_CONFIG_ADDR).
 * @param out_boot_cmd_id   Receives the host -> boot ID (default 0x79E).
 * @param out_boot_reply_id Receives the boot -> host ID (default 0x79F).
 */
void dfu_config_can_ids(const uint8_t *blob,
                        uint16_t *out_boot_cmd_id,
                        uint16_t *out_boot_reply_id);

#ifdef __cplusplus
}
#endif

#endif /* DFU_CONFIG_H */
