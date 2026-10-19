/* SPDX-FileCopyrightText: 2015-2021 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
/**
 * @file    boot.c
 * @brief   Node recovery selection before IDF touches otadata or loads an app.
 *
 * Overrides the ESP-IDF 2nd-stage bootloader entry (call_start_cpu0) to classify the reset,
 * advance the recovery counters and, once either trips, load the factory CAN updater. Based on
 * ESP-IDF v5.3.6 examples/custom_bootloader/bootloader_override.
 *
 * Owns:         The bootloader's copy of the recovery counters: the RTC retained record and the
 *               bootcnt flash log.
 * Not here:     The counter logic (src/boot/boot_counter.h); clearing the counters
 *               (components/can_updater_client/recovery.c). IDF retains responsibility for image
 *               verification, rollback and jumping.
 * Invariants:   Only the dedicated writable, unencrypted bootcnt data partition is written, never
 *               app/otadata. A bootcnt failure never blocks the boot: the RTC decision still
 *               applies.
 * Depends on:   boot_counter.h and the IDF bootloader support components.
 *
 * @see     boot_counter.h, docs/concepts/recovery.md
 */
#include "boot_counter.h"

#include <string.h>
#include "bootloader_common.h"
#include "bootloader_flash_priv.h"
#include "bootloader_init.h"
#include "bootloader_utility.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "soc/reset_reasons.h"

static const char *TAG = "recovery";
static uint32_t s_log[BOOT_COUNTER_FLASH_BYTES / sizeof(uint32_t)];

_Static_assert(CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC_SIZE == sizeof(boot_counter_rtc_t),
               "Bootloader and application RTC contracts must match");
#if CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC_IN_CRC
#error "Custom RTC state has its own check value"
#endif

static boot_counter_reset_t classify_reset(void)
{
    switch (esp_rom_get_reset_reason(0)) {
    case RESET_REASON_CHIP_POWER_ON: return BOOT_COUNTER_POWER_ON;
    case RESET_REASON_CPU0_SW:
    case RESET_REASON_CORE_SW: return BOOT_COUNTER_SOFTWARE;
    case RESET_REASON_CORE_DEEP_SLEEP: return BOOT_COUNTER_SLEEP;
    default: return BOOT_COUNTER_CRASH;
    }
}

/* Table has already passed IDF's bounds/MD5 validation. Only the dedicated
 * writable, unencrypted data partition may be touched, never app/otadata. */
static uint32_t counter_offset(void)
{
    esp_partition_info_t entry;
    for (size_t i = 0; i < ESP_PARTITION_TABLE_MAX_ENTRIES; ++i) {
        if (bootloader_flash_read(ESP_PARTITION_TABLE_OFFSET + i * sizeof(entry),
                                  &entry, sizeof(entry), true) != ESP_OK) {
            break;
        }
        if (entry.magic != ESP_PARTITION_MAGIC) {
            break;
        }
        if (entry.type == PART_TYPE_DATA && entry.subtype == BOOT_COUNTER_PARTITION_SUBTYPE &&
            memcmp(entry.label, BOOT_COUNTER_PARTITION_LABEL, sizeof(BOOT_COUNTER_PARTITION_LABEL)) == 0 &&
            entry.pos.size == BOOT_COUNTER_FLASH_BYTES && entry.flags == 0u) {
            return entry.pos.offset;
        }
    }
    return 0u;
}

static bool append_record(uint32_t offset, size_t slot)
{
    uint32_t verify[BOOT_COUNTER_RECORD_BYTES / sizeof(uint32_t)];
    const uint32_t address = offset + (uint32_t)slot;
    if (bootloader_flash_write(address, (void *)boot_counter_record,
                               BOOT_COUNTER_RECORD_BYTES, false) != ESP_OK ||
        bootloader_flash_read(address, verify, sizeof(verify), false) != ESP_OK ||
        memcmp(verify, boot_counter_record, sizeof(verify)) != 0) {
        ESP_LOGW(TAG, "bootcnt write/verify failed; continuing boot");
        return false;
    }
    memcpy((uint8_t *)s_log + slot, verify, sizeof(verify));
    return true;
}

static bool flash_counter(bool power_on, bool rtc_trip)
{
    const uint32_t offset = counter_offset();
    if (offset == 0u || bootloader_flash_read(offset, s_log, sizeof(s_log), false) != ESP_OK) {
        ESP_LOGW(TAG, "bootcnt unavailable; continuing with RTC decision");
        return false;
    }
    boot_counter_log_t log = boot_counter_log_scan(s_log, sizeof(s_log));
    if (log.count >= BOOT_COUNTER_LIMIT) {
        return true;
    }
    if (!power_on && !rtc_trip) {
        return false;
    }
    /* Persist an RTC trip too: EN must not release an already latched recovery.
     * Normally this loop writes just one 16-byte record; at most three writes. */
    const uint32_t target = rtc_trip ? BOOT_COUNTER_LIMIT : log.count + 1u;
    if (boot_counter_log_wrap(&log)) {
        /* Only a full, UNTRIPPED log wraps. Interrupted erase loses evidence,
         * never damages an image; uncertain evidence must fail normal. */
        if (bootloader_flash_erase_range(offset, sizeof(s_log)) != ESP_OK ||
            bootloader_flash_read(offset, s_log, sizeof(s_log), false) != ESP_OK ||
            boot_counter_log_scan(s_log, sizeof(s_log)).dirty) {
            ESP_LOGW(TAG, "bootcnt wrap failed; continuing boot");
            return false;
        }
        log = boot_counter_log_scan(s_log, sizeof(s_log));
    }
    while (log.count < target && log.next_slot != BOOT_COUNTER_NO_SLOT) {
        if (!append_record(offset, log.next_slot)) {
            break;
        }
        log = boot_counter_log_scan(s_log, sizeof(s_log));
    }
    ESP_LOGI(TAG, "power/EN count %lu", (unsigned long)log.count);
    return log.count >= BOOT_COUNTER_LIMIT;
}

void __attribute__((noreturn)) call_start_cpu0(void)
{
    if (bootloader_init() != ESP_OK) {
        bootloader_reset();
    }
    bootloader_state_t state = {0};
    if (!bootloader_utility_load_partition_table(&state)) {
        bootloader_reset();
    }
    const boot_counter_reset_t reset = classify_reset();
    boot_counter_rtc_t rtc;
    memcpy(&rtc, bootloader_common_get_rtc_retain_mem()->custom, sizeof(rtc));
    const bool rtc_trip = boot_counter_rtc_boot(&rtc, reset);
    /* Establish IDF's independent CRC before load_image updates its counter.
     * Otherwise its invalid-CRC path would zero our freshly written state. */
    bootloader_common_update_rtc_retain_mem(NULL, true);
    memcpy(bootloader_common_get_rtc_retain_mem()->custom, &rtc, sizeof(rtc));
    const bool flash_trip = flash_counter(reset == BOOT_COUNTER_POWER_ON, rtc_trip);
    int index;
    if ((rtc_trip || flash_trip) && state.factory.size != 0u) {
        ESP_LOGW(TAG, "latched recovery -> factory (RTC %lu, flash %u)",
                 (unsigned long)rtc.count, (unsigned)flash_trip);
        /* Do NOT call get_selected here: it can update pending OTA metadata. */
        index = FACTORY_INDEX;
    } else {
        index = bootloader_utility_get_selected_boot_partition(&state);
    }
    if (index == INVALID_INDEX) {
        bootloader_reset();
    }
    bootloader_utility_load_boot_image(&state, index);
}

struct _reent *__getreent(void)
{
    return _GLOBAL_REENT;
}
