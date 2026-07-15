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

/* -----------------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------------- */
dfu_config_status_t dfu_config_parse(const uint8_t *blob,
                                     const char **out_mfr, uint8_t *out_mfr_len,
                                     const char **out_prod, uint8_t *out_prod_len)
{
    if (load_le32(&blob[DFU_CONFIG_OFF_MAGIC]) != DFU_CONFIG_MAGIC) {
        return DFU_CFG_ERR_MAGIC;
    }
    if (blob[DFU_CONFIG_OFF_VERSION] != DFU_CONFIG_VERSION) {
        return DFU_CFG_ERR_VERSION;
    }

    const uint8_t mfr_len  = blob[DFU_CONFIG_OFF_MFR_LEN];
    const uint8_t prod_len = blob[DFU_CONFIG_OFF_PROD_LEN];
    if (mfr_len > DFU_CONFIG_STR_MAX || prod_len > DFU_CONFIG_STR_MAX) {
        return DFU_CFG_ERR_LEN;
    }

    if (crc32_ieee(blob, DFU_CONFIG_CRC_COVERAGE) !=
        load_le32(&blob[DFU_CONFIG_OFF_CRC])) {
        return DFU_CFG_ERR_CRC;
    }

    *out_mfr      = (const char *)&blob[DFU_CONFIG_OFF_MFR];
    *out_mfr_len  = mfr_len;
    *out_prod     = (const char *)&blob[DFU_CONFIG_OFF_PROD];
    *out_prod_len = prod_len;
    return DFU_CFG_OK;
}
