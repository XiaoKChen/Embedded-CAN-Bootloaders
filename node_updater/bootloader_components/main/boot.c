/* SPDX-FileCopyrightText: 2015-2021 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
/**
 * @file    boot.c
 * @brief   Board safety holds and recovery selection before IDF loads an app.
 *
 * Overrides the ESP-IDF 2nd-stage bootloader entry (call_start_cpu0) to classify the reset,
 * apply validated cancfg GPIO holds (LOW and HIGH) and the profile's I2C safe writes immediately
 * after initialization (before table logging), advance recovery counters and, once either trips,
 * load the factory CAN updater. Based on
 * ESP-IDF v5.3.6 examples/custom_bootloader/bootloader_override.
 *
 * Owns:         The bootloader's copy of the recovery counters: the RTC retained record and the
 *               bootcnt flash log; the bootloader's GPIO-register HAL for i2c_bitbang.
 * Not here:     The counter logic (src/boot/boot_counter.h); clearing the counters
 *               (components/can_updater_client/recovery.c); the record (board_profile.h) and the
 *               I2C sequence (i2c_bitbang.h). IDF retains responsibility for image verification,
 *               rollback and jumping.
 * Invariants:   Only the dedicated writable, unencrypted bootcnt data partition is written, never
 *               app/otadata. A bootcnt failure never blocks the boot: the RTC decision still
 *               applies. The I2C safe writes are best effort and bounded (I2C_BB_WORST_CASE_US):
 *               a failure is skipped, and both I2C pins are left released inputs for the app.
 * Depends on:   boot_counter.h, board_profile.h, i2c_bitbang.h and the IDF bootloader support
 *               components.
 *
 * @see     boot_counter.h, i2c_bitbang.h, docs/concepts/recovery.md,
 *          docs/reference/board-profile.md
 */
#include "boot_counter.h"
#include "board_profile.h"
#include "i2c_bitbang.h"
#include "../../cancfg.h"

#include <string.h>
#include "bootloader_common.h"
#include "bootloader_flash_priv.h"
#include "bootloader_init.h"
#include "bootloader_utility.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_rom_gpio.h"
#include "hal/gpio_ll.h"
#include "soc/gpio_sig_map.h"
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

/* i2c_bitbang pin HAL over the GPIO registers: the pins are open-drain, so a released line
 * (output latch 1) is pulled up and only LOW is ever driven. ctx points at {sda, scl}. */
static void i2c_line_set(uint32_t pin, bool release)
{
    gpio_ll_set_level(&GPIO, pin, release ? 1u : 0u);
}

static void i2c_sda_release(void *ctx, bool release)
{
    i2c_line_set(((const uint8_t *)ctx)[0], release);
}

static void i2c_scl_release(void *ctx, bool release)
{
    i2c_line_set(((const uint8_t *)ctx)[1], release);
}

static bool i2c_sda_read(void *ctx)
{
    return gpio_ll_get_level(&GPIO, ((const uint8_t *)ctx)[0]) != 0;
}

static bool i2c_scl_read(void *ctx)
{
    return gpio_ll_get_level(&GPIO, ((const uint8_t *)ctx)[1]) != 0;
}

static void i2c_delay_us(void *ctx, uint32_t us)
{
    (void)ctx;
    esp_rom_delay_us(us);
}

static void i2c_line_open_drain(uint32_t pin)
{
    gpio_ll_set_level(&GPIO, pin, 1u);  /* released before the driver is enabled */
    esp_rom_gpio_pad_select_gpio(pin);
    esp_rom_gpio_pad_pullup_only(pin);
    gpio_ll_input_enable(&GPIO, pin);
    gpio_ll_od_enable(&GPIO, pin);
    esp_rom_gpio_connect_out_signal(pin, SIG_GPIO_OUT_IDX, false, false);
    gpio_ll_output_enable(&GPIO, pin);
}

static void i2c_line_release(uint32_t pin)
{
    gpio_ll_output_disable(&GPIO, pin);
    gpio_ll_od_disable(&GPIO, pin);
}

/* Best effort: a missing or wedged chip costs at most I2C_BB_WORST_CASE_US, never the boot. */
static void apply_i2c_safe_writes(const board_profile_t *profile)
{
    if (profile->i2c_sda_gpio == BOARD_PROFILE_I2C_NONE) {
        return;
    }
    uint8_t pins[2] = {profile->i2c_sda_gpio, profile->i2c_scl_gpio};
    const i2c_bb_hal_t hal = {
        pins, i2c_sda_release, i2c_scl_release, i2c_sda_read, i2c_scl_read, i2c_delay_us,
    };
    i2c_line_open_drain(pins[0]);
    i2c_line_open_drain(pins[1]);
    const uint8_t acked = i2c_bb_apply_safe_writes(&hal, profile);
    i2c_line_release(pins[0]);
    i2c_line_release(pins[1]);
    ESP_LOGI(TAG, "I2C safe writes 0x%02X: %u/%u ACKed", (unsigned)profile->i2c_addr7,
             (unsigned)acked, (unsigned)profile->i2c_write_count);
}

/* Verify silently before the regular table loader prints its UART table.
 * No flash mutation, heap, RTOS or application driver calls in this path. */
static void apply_board_holds(void)
{
    const esp_partition_info_t *table = bootloader_mmap(
        ESP_PARTITION_TABLE_OFFSET, ESP_PARTITION_TABLE_MAX_LEN);
    if (table == NULL) {
        return;
    }
    int num_partitions;
    const esp_err_t verified = esp_partition_table_verify(table, false, &num_partitions);
    bootloader_munmap(table);
    if (verified != ESP_OK) {
        return;  /* The normal loader below will reject the damaged table. */
    }
    esp_partition_info_t entry;
    for (size_t i = 0; i < ESP_PARTITION_TABLE_MAX_ENTRIES; ++i) {
        if (bootloader_flash_read(ESP_PARTITION_TABLE_OFFSET + i * sizeof(entry),
                                   &entry, sizeof(entry), true) != ESP_OK ||
            entry.magic != ESP_PARTITION_MAGIC) {
            return;
        }
        if (entry.type != PART_TYPE_DATA || entry.subtype != CAN_CFG_PARTITION_SUBTYPE ||
            memcmp(entry.label, CAN_CFG_PARTITION_LABEL, sizeof(CAN_CFG_PARTITION_LABEL)) != 0 ||
            entry.pos.size < BOARD_PROFILE_CANCFG_OFFSET + BOARD_PROFILE_SIZE || entry.flags != 0u) {
            continue;
        }
        uint32_t record[BOARD_PROFILE_SIZE / sizeof(uint32_t)];
        board_profile_t profile;
        if (bootloader_flash_read(entry.pos.offset + BOARD_PROFILE_CANCFG_OFFSET,
                                   record, sizeof(record), false) != ESP_OK ||
            board_profile_parse((const uint8_t *)record, sizeof(record), &profile) != BOARD_PROFILE_OK) {
            return;  /* Older provisioning has no holds. */
        }
        const uint64_t held = profile.hold_low_mask | profile.hold_high_mask;
        for (uint32_t pin = 0u; pin <= BOARD_PROFILE_MAX_GPIO; ++pin) {
            if ((held & (1ull << pin)) != 0u) {
                /* GPIO_OUT latch precedes mux/output enable to avoid a pulse of the wrong level. */
                gpio_ll_set_level(&GPIO, pin, (profile.hold_high_mask & (1ull << pin)) != 0u ? 1u : 0u);
                esp_rom_gpio_pad_select_gpio(pin);
                esp_rom_gpio_connect_out_signal(pin, SIG_GPIO_OUT_IDX, false, false);
                gpio_ll_output_enable(&GPIO, pin);
            }
        }
        apply_i2c_safe_writes(&profile);
        return;
    }
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
    apply_board_holds();
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
