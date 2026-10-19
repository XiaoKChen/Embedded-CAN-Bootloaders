/**
 * @file    recovery.c
 * @brief   Explicit app-side health tracking for the node bootloader (see can_updater_client.h for
 *          the contract).
 *
 * Implementation notes: a shutdown handler sets the one-use clean-restart mark in RTC retained
 * memory; a static task pinned to CPU0 (which owns that memory) clears both counters after
 * BOOT_COUNTER_STABLE_MS. Clears are serialized by a mutex and read-back verified, and a latched
 * trip is cleared only by the factory updater.
 *
 * @see     can_updater_client.h, boot_counter.h, node_updater/bootloader_components/main/boot.c
 */
#include "can_updater_client.h"

#include <string.h>
#include "boot_counter.h"
#include "bootloader_common.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#ifndef CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC
#error "Enable BOOTLOADER_CUSTOM_RESERVE_RTC with size 0x10 in the app defaults"
#endif
#if CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC_IN_CRC
#error "Custom RTC data must be excluded from the IDF CRC"
#endif
_Static_assert(CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC_SIZE == sizeof(boot_counter_rtc_t),
               "App and node bootloader RTC layout must match");

#define HEALTH_STACK_BYTES 2048u
static const char *TAG = "node_health";
static StaticTask_t s_task;
static StackType_t s_stack[HEALTH_STACK_BYTES / sizeof(StackType_t)];
static uint32_t s_log[BOOT_COUNTER_FLASH_BYTES / sizeof(uint32_t)];
static portMUX_TYPE s_rtc_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_started;
static StaticSemaphore_t s_clear_mutex_storage;
static SemaphoreHandle_t s_clear_mutex;

static void clean_restart(void)
{
    boot_counter_rtc_t rtc;
    portENTER_CRITICAL(&s_rtc_lock);
    memcpy(&rtc, bootloader_common_get_rtc_retain_mem()->custom, sizeof(rtc));
    boot_counter_rtc_clean_restart(&rtc);
    memcpy(bootloader_common_get_rtc_retain_mem()->custom, &rtc, sizeof(rtc));
    portEXIT_CRITICAL(&s_rtc_lock);
}

static esp_err_t clear_counters_locked(void)
{
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, BOOT_COUNTER_PARTITION_SUBTYPE, BOOT_COUNTER_PARTITION_LABEL);
    if (part == NULL || part->size != sizeof(s_log) || part->encrypted || part->readonly ||
        esp_partition_read(part, 0, s_log, sizeof(s_log)) != ESP_OK) {
        ESP_LOGW(TAG, "bootcnt unavailable; counters not cleared");
        return ESP_FAIL;
    }
    boot_counter_rtc_t rtc;
    portENTER_CRITICAL(&s_rtc_lock);
    memcpy(&rtc, bootloader_common_get_rtc_retain_mem()->custom, sizeof(rtc));
    portEXIT_CRITICAL(&s_rtc_lock);
    const boot_counter_log_t log = boot_counter_log_scan(s_log, sizeof(s_log));
    const bool tripped = log.count >= BOOT_COUNTER_LIMIT ||
                        (boot_counter_rtc_valid(&rtc) && rtc.count >= BOOT_COUNTER_LIMIT);
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (tripped && (running == NULL || running->subtype != ESP_PARTITION_SUBTYPE_APP_FACTORY)) {
        ESP_LOGW(TAG, "latched recovery may only be cleared by factory");
        return ESP_ERR_INVALID_STATE;
    }
    if (log.dirty &&
        (esp_partition_erase_range(part, 0, part->size) != ESP_OK ||
         esp_partition_read(part, 0, s_log, sizeof(s_log)) != ESP_OK ||
         boot_counter_log_scan(s_log, sizeof(s_log)).dirty)) {
        ESP_LOGW(TAG, "bootcnt clear/verify failed; RTC counter retained");
        return ESP_FAIL;
    }
    portENTER_CRITICAL(&s_rtc_lock);
    boot_counter_rtc_clear(&rtc);
    memcpy(bootloader_common_get_rtc_retain_mem()->custom, &rtc, sizeof(rtc));
    portEXIT_CRITICAL(&s_rtc_lock);
    return ESP_OK;
}

static esp_err_t clear_counters(void)
{
    if (xSemaphoreTake(s_clear_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    const esp_err_t err = clear_counters_locked();
    if (xSemaphoreGive(s_clear_mutex) != pdTRUE) {
        return ESP_FAIL;
    }
    return err;
}

esp_err_t can_updater_client_prepare_verified_boot(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!s_started || running == NULL || running->subtype != ESP_PARTITION_SUBTYPE_APP_FACTORY) {
        return ESP_ERR_INVALID_STATE;
    }
    return clear_counters();
}

static void health_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(BOOT_COUNTER_STABLE_MS));
    const esp_err_t err = clear_counters();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "10 s stable uptime: recovery counters cleared");
    } else {
        ESP_LOGW(TAG, "stable clear failed: %s", esp_err_to_name(err));
    }
    for (;;) {
        vTaskSuspend(NULL);
    }
}

esp_err_t can_updater_client_start_health(void)
{
    if (s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    s_clear_mutex = xSemaphoreCreateMutexStatic(&s_clear_mutex_storage);
    if (s_clear_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    const esp_err_t err = esp_register_shutdown_handler(clean_restart);
    if (err != ESP_OK) {
        return err;
    }
    /* Static storage, CPU0: the retained FAST memory is owned by PRO_CPU. */
    if (xTaskCreateStaticPinnedToCore(health_task, "node_health", sizeof(s_stack), NULL,
                                    tskIDLE_PRIORITY + 1, s_stack, &s_task, 0) == NULL) {
        const esp_err_t unregister_err = esp_unregister_shutdown_handler(clean_restart);
        return unregister_err == ESP_OK ? ESP_ERR_NO_MEM : unregister_err;
    }
    s_started = true;
    return ESP_OK;
}
