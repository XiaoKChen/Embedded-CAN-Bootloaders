/**
 * @file    ota_select.h
 * @brief   Which OTA slot the ESP-IDF bootloader would start from the two otadata entries (pure logic).
 *
 * Mirrors bootloader_utility_get_selected_boot_partition (ESP-IDF v5.3, app rollback enabled, no
 * anti-rollback) for the EXIT_UPDATER check: PENDING_VERIFY entries are first turned ABORTED (the
 * bootloader writes that before it selects), then entries with ota_seq 0xFFFFFFFF, a bad CRC, or
 * state INVALID/ABORTED are discarded, the larger ota_seq of the remaining entries wins, and the
 * slot is (ota_seq - 1) % app_count. NEW stays selectable. An unconfirmed image with an older VALID
 * entry therefore loses to that entry (EXIT rolls back to it).
 *
 * Owns:         The otadata entry layout and the selection rule.
 * Not here:     Reading otadata, finding the slot's partition and the image check
 *               (esp_image_verify) -- node_updater/main/main.c.
 * Invariants:   The sequence is compared as plain uint32 like the bootloader, with no wrap-aware
 *               ordering. The entry layout is the 32-byte esp_ota_select_entry_t.
 * Depends on:   crc32.h only. No hardware, no heap.
 *
 * @see     ota_select.c, test_ota_select.c, can_update.h
 */

#ifndef OTA_SELECT_H
#define OTA_SELECT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OTA_SELECT_ENTRY_COUNT      2u
#define OTA_SELECT_STATE_NEW        0x00000000u
#define OTA_SELECT_STATE_PENDING    0x00000001u
#define OTA_SELECT_STATE_VALID      0x00000002u
#define OTA_SELECT_STATE_INVALID    0x00000003u
#define OTA_SELECT_STATE_ABORTED    0x00000004u
#define OTA_SELECT_STATE_UNDEFINED  0xFFFFFFFFu
#define OTA_SELECT_SEQ_ERASED       0xFFFFFFFFu

/* Same layout as esp_ota_select_entry_t (little-endian target and host). */
typedef struct {
    uint32_t ota_seq;
    uint8_t  seq_label[20];
    uint32_t ota_state;
    uint32_t crc;           /* CRC32 (zlib) of the four ota_seq bytes */
} ota_select_entry_t;

/** CRC the bootloader expects in `crc` for this ota_seq. */
uint32_t ota_select_crc(uint32_t ota_seq);

/**
 * OTA slot index (0-based) the bootloader would boot, or -1 when it would fall back to factory
 * (no usable entry, or app_count 0). Does not modify `entries`.
 */
int ota_select_boot_slot(const ota_select_entry_t entries[OTA_SELECT_ENTRY_COUNT], uint32_t app_count);

#ifdef __cplusplus
}
#endif

#endif /* OTA_SELECT_H */
