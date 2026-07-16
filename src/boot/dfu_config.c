/* =============================================================================
 * dfu_config.c — Pure-logic parse/validate of the flash-time DFU name config
 * =============================================================================
 * See dfu_config.h for the interface and the frozen block layout. Fail-closed:
 * every field is checked before the out-params are populated, and the string
 * bytes are exposed only after the crc32 authenticates the whole record. All
 * multi-byte reads go through explicit little-endian byte loads — no aliased
 * direct loads from the buffer (mirrors image_validate.c).
 * ============================================================================= */

#include "dfu_config.h"

#include "boot_config.h"  /* CAN_ID_HOST_TO_BOOT / CAN_ID_BOOT_TO_HOST defaults */
#include "crc32.h"

/* -----------------------------------------------------------------------------
 * Private helpers
 * ----------------------------------------------------------------------------- */
static uint32_t load_le32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint16_t load_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/* Validate magic, version and the version's crc32 coverage. On DFU_CFG_OK,
 * *out_version holds the accepted version byte. Single source of the fail-closed
 * checks shared by both public accessors below. */
static dfu_config_status_t dfu_config_validate(const uint8_t *blob,
                                               uint8_t *out_version)
{
    if (load_le32(&blob[DFU_CONFIG_OFF_MAGIC]) != DFU_CONFIG_MAGIC) {
        return DFU_CFG_ERR_MAGIC;
    }

    const uint8_t version = blob[DFU_CONFIG_OFF_VERSION];
    uint32_t crc_coverage;
    uint32_t crc_offset;
    if (version == DFU_CONFIG_VERSION_V1) {
        crc_coverage = DFU_CONFIG_CRC_COVERAGE_V1;
        crc_offset   = DFU_CONFIG_OFF_CRC_V1;
    } else if (version == DFU_CONFIG_VERSION_V2) {
        crc_coverage = DFU_CONFIG_CRC_COVERAGE_V2;
        crc_offset   = DFU_CONFIG_OFF_CRC_V2;
    } else {
        return DFU_CFG_ERR_VERSION;
    }

    if (blob[DFU_CONFIG_OFF_MFR_LEN] > DFU_CONFIG_STR_MAX ||
        blob[DFU_CONFIG_OFF_PROD_LEN] > DFU_CONFIG_STR_MAX) {
        return DFU_CFG_ERR_LEN;
    }

    if (crc32_ieee(blob, crc_coverage) != load_le32(&blob[crc_offset])) {
        return DFU_CFG_ERR_CRC;
    }

    *out_version = version;
    return DFU_CFG_OK;
}

/* -----------------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------------- */
dfu_config_status_t dfu_config_parse(const uint8_t *blob,
                                     const char **out_mfr, uint8_t *out_mfr_len,
                                     const char **out_prod, uint8_t *out_prod_len)
{
    uint8_t version;
    const dfu_config_status_t status = dfu_config_validate(blob, &version);
    if (status != DFU_CFG_OK) {
        return status;
    }

    *out_mfr      = (const char *)&blob[DFU_CONFIG_OFF_MFR];
    *out_mfr_len  = blob[DFU_CONFIG_OFF_MFR_LEN];
    *out_prod     = (const char *)&blob[DFU_CONFIG_OFF_PROD];
    *out_prod_len = blob[DFU_CONFIG_OFF_PROD_LEN];
    return DFU_CFG_OK;
}

void dfu_config_can_ids(const uint8_t *blob,
                        uint16_t *out_boot_cmd_id,
                        uint16_t *out_boot_reply_id)
{
    /* Default first, so every return path (absent/invalid/v1) yields usable
     * IDs; a valid v2 block overrides each ID below when it is in range. */
    *out_boot_cmd_id   = (uint16_t)CAN_ID_HOST_TO_BOOT;
    *out_boot_reply_id = (uint16_t)CAN_ID_BOOT_TO_HOST;

    uint8_t version;
    if (dfu_config_validate(blob, &version) != DFU_CFG_OK ||
        version != DFU_CONFIG_VERSION_V2) {
        return;
    }

    const uint16_t cmd_id   = load_le16(&blob[DFU_CONFIG_OFF_BOOT_CMD_ID]);
    const uint16_t reply_id = load_le16(&blob[DFU_CONFIG_OFF_BOOT_REPLY_ID]);
    if (cmd_id <= DFU_CONFIG_CAN_ID_MAX) {
        *out_boot_cmd_id = cmd_id;
    }
    if (reply_id <= DFU_CONFIG_CAN_ID_MAX) {
        *out_boot_reply_id = reply_id;
    }
}
