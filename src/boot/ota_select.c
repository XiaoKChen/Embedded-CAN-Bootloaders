/**
 * @file    ota_select.c
 * @brief   Bootloader-equivalent otadata slot selection (see ota_select.h for the contract).
 *
 * Implementation notes: the rule is bootloader_common_get_active_otadata plus the PENDING_VERIFY ->
 * ABORTED step, copied from ESP-IDF v5.3.
 *
 * @see     ota_select.h, test_ota_select.c
 */

#include "ota_select.h"

#include <stdbool.h>

#include "crc32.h"

uint32_t ota_select_crc(uint32_t ota_seq)
{
    const uint8_t bytes[4] = {
        (uint8_t)(ota_seq & 0xFFu), (uint8_t)((ota_seq >> 8) & 0xFFu),
        (uint8_t)((ota_seq >> 16) & 0xFFu), (uint8_t)((ota_seq >> 24) & 0xFFu),
    };
    /* The bootloader runs the ROM crc32_le with init UINT32_MAX, which is zlib continued from it. */
    return crc32_ieee_update(UINT32_MAX, bytes, sizeof(bytes));
}

/* state is the entry's state after the PENDING_VERIFY -> ABORTED rule. */
static bool entry_selectable(const ota_select_entry_t *e, uint32_t state)
{
    return e->ota_seq != OTA_SELECT_SEQ_ERASED && state != OTA_SELECT_STATE_INVALID &&
           state != OTA_SELECT_STATE_ABORTED && e->crc == ota_select_crc(e->ota_seq);
}

int ota_select_boot_slot(const ota_select_entry_t entries[OTA_SELECT_ENTRY_COUNT], uint32_t app_count)
{
    if (app_count == 0u) {
        return -1;
    }
    bool selectable[OTA_SELECT_ENTRY_COUNT];
    for (uint32_t i = 0u; i < OTA_SELECT_ENTRY_COUNT; ++i) {
        const uint32_t state = (entries[i].ota_state == OTA_SELECT_STATE_PENDING)
                                   ? OTA_SELECT_STATE_ABORTED : entries[i].ota_state;
        selectable[i] = entry_selectable(&entries[i], state);
    }

    int active = -1;
    if (selectable[0] && selectable[1]) {
        active = (entries[0].ota_seq >= entries[1].ota_seq) ? 0 : 1;
    } else if (selectable[0]) {
        active = 0;
    } else if (selectable[1]) {
        active = 1;
    }
    if (active < 0) {
        return -1;
    }
    return (int)((entries[active].ota_seq - 1u) % app_count);
}
