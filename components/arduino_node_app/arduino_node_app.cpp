/**
 * @file    arduino_node_app.cpp
 * @brief   CAN update node-app implementation (see arduino_node_app.hpp for the contract).
 *
 * Implementation notes: ports the SOLO ODU's bench-proven flash counter and app-control path,
 * without recovery.c's custom RTC dependency. The RX path only posts notification bits; the
 * update task uses a bounded twai_transmit() wait per frame so a Cyphal publish burst cannot
 * truncate APP_INFO merely because the TX queue is full at receipt. ENTER_UPDATER ACKs, drains,
 * selects factory and restarts. GPIO latches are cleared before enabling profile outputs.
 * Image confirmation is caller-gated; the app owns Arduino's pre-setup verifyRollbackLater hook.
 *
 * @see     arduino_node_app.hpp, app_control.h, board_profile.h, boot_counter.h
 */

#include "arduino_node_app.hpp"

#include <Arduino.h>
#include <app_control.h>
#include <atomic>
#include <board_profile.h>
#include <cstring>
#include <dfu_config.h>
#include <driver/gpio.h>
#include <driver/twai.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <freertos/task.h>

namespace arduino_node_app {
namespace {

constexpr const char *kCanCfgPartitionLabel = "cancfg";
constexpr auto kCanCfgPartitionSubtype = static_cast<esp_partition_subtype_t>(0x40);
constexpr uint32_t kRequestAppInfo      = 1u << 0;
constexpr uint32_t kRequestEnterUpdater = 1u << 1;
constexpr uint32_t kRequestMalformed    = 1u << 2;
constexpr uint32_t kAllRequests = kRequestAppInfo | kRequestEnterUpdater | kRequestMalformed;

Config settings{};
uint16_t update_cmd_id   = CAN_ID_HOST_TO_BOOT;
uint16_t update_reply_id = CAN_ID_BOOT_TO_HOST;
const char *firmware_label = nullptr;
const char *firmware_version = nullptr;
#ifdef USB_PRODUCT
char firmware_product[] = USB_PRODUCT;
#else
char firmware_product[] = "";
#endif

// The sector and task storage never occupy the app's stack or heap.
uint32_t boot_counter_sector[BOOT_COUNTER_FLASH_BYTES / sizeof(uint32_t)];
StaticTask_t update_task_storage;
StackType_t update_task_stack[kDefaultTaskStackSizeBytes / sizeof(StackType_t)];
TaskHandle_t update_task_handle = nullptr;
std::atomic<uint8_t> malformed_cmd{0};

void load_firmware_identity()
{
    firmware_label = firmware_product;
    firmware_version = firmware_product;
    char *version_sep = strrchr(firmware_product, ' ');
    if (version_sep != nullptr)
    {
        firmware_version = version_sep + 1;
        *version_sep = '\0';
        while (version_sep > firmware_product && version_sep[-1] == ' ')
            *--version_sep = '\0';
    }
    if (settings.label != nullptr)
        firmware_label = settings.label;
    if (settings.version != nullptr)
        firmware_version = settings.version;
}

// Unreadable data must look erased to the shared parsers, exactly as on an
// unprovisioned board. Profile validity is independent of the v2 ID block.
esp_err_t load_config()
{
    uint8_t block[DFU_CONFIG_SIZE];
    memset(block, 0xFF, sizeof(block));
    const esp_partition_t *cfg =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, kCanCfgPartitionSubtype, kCanCfgPartitionLabel);
    if (cfg == nullptr || esp_partition_read(cfg, 0, block, sizeof(block)) != ESP_OK)
    {
        memset(block, 0xFF, sizeof(block));
        Serial.println("can_update: cancfg partition unreadable; using the default CAN IDs");
    }
    dfu_config_can_ids(block, &update_cmd_id, &update_reply_id);

    uint8_t record[BOARD_PROFILE_SIZE];
    memset(record, 0xFF, sizeof(record));
    if (cfg == nullptr ||
        esp_partition_read(cfg, BOARD_PROFILE_CANCFG_OFFSET, record, sizeof(record)) != ESP_OK)
    {
        memset(record, 0xFF, sizeof(record));
    }
    board_profile_t profile{};
    if (board_profile_parse(record, sizeof(record), &profile) != BOARD_PROFILE_OK)
    {
        Serial.println("can_update: board profile absent/invalid; using default (no LOW holds)");
    }
    if (profile.hold_low_mask == 0)
        return ESP_OK;

    for (uint8_t pin = 0; pin <= BOARD_PROFILE_MAX_GPIO; ++pin)
    {
        if ((profile.hold_low_mask & (1ull << pin)) == 0)
            continue;
        const esp_err_t err = gpio_set_level(static_cast<gpio_num_t>(pin), 0);
        if (err != ESP_OK)
            return err;
    }
    gpio_config_t outputs{};
    outputs.pin_bit_mask = profile.hold_low_mask;
    outputs.mode = GPIO_MODE_OUTPUT;
    outputs.pull_up_en = GPIO_PULLUP_DISABLE;
    outputs.pull_down_en = GPIO_PULLDOWN_DISABLE;
    outputs.intr_type = GPIO_INTR_DISABLE;
    return gpio_config(&outputs);
}

void log_running_image()
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    const bool pending = running != nullptr && esp_ota_get_state_partition(running, &state) == ESP_OK &&
                         state == ESP_OTA_IMG_PENDING_VERIFY;
    Serial.printf("can_update: %s %s running from %s%s; update IDs 0x%03X/0x%03X\n", firmware_label,
                  firmware_version, running != nullptr ? running->label : "?", pending ? " (PENDING_VERIFY)" : "",
                  (unsigned)update_cmd_id, (unsigned)update_reply_id);
}

enum class BootCounterAction : uint8_t
{
    NothingToDo,
    Clear,
    KeepTripped,
};

// Erase torn records too, but never clear latched recovery outside factory.
BootCounterAction boot_counter_action(const boot_counter_log_t &log)
{
    if (log.count >= BOOT_COUNTER_LIMIT)
        return BootCounterAction::KeepTripped;
    return log.dirty ? BootCounterAction::Clear : BootCounterAction::NothingToDo;
}

void clear_boot_counter()
{
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, static_cast<esp_partition_subtype_t>(BOOT_COUNTER_PARTITION_SUBTYPE),
        BOOT_COUNTER_PARTITION_LABEL);
    if (part == nullptr || part->size != sizeof(boot_counter_sector) || part->encrypted || part->readonly ||
        esp_partition_read(part, 0, boot_counter_sector, sizeof(boot_counter_sector)) != ESP_OK)
    {
        Serial.println("can_update: bootcnt unavailable; boot counter not cleared");
        return;
    }

    switch (boot_counter_action(boot_counter_log_scan(boot_counter_sector, sizeof(boot_counter_sector))))
    {
        case BootCounterAction::NothingToDo:
            Serial.println("can_update: stable uptime: boot counter already clear");
            return;
        case BootCounterAction::KeepTripped:
            Serial.println("can_update: boot counter tripped; latched recovery may only be cleared by factory");
            return;
        case BootCounterAction::Clear:
            break;
    }

    if (esp_partition_erase_range(part, 0, part->size) != ESP_OK ||
        esp_partition_read(part, 0, boot_counter_sector, sizeof(boot_counter_sector)) != ESP_OK ||
        boot_counter_log_scan(boot_counter_sector, sizeof(boot_counter_sector)).dirty)
    {
        Serial.println("can_update: bootcnt clear/verify failed");
        return;
    }
    Serial.println("can_update: stable uptime: boot counter cleared");
}

// Selecting factory erases otadata, keeping the updater parked across resets
// until an image is flashed. No RTC clean-restart mark on the stock core.
esp_err_t enter_updater()
{
    const esp_partition_t *updater =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, nullptr);
    if (updater == nullptr)
        return ESP_ERR_NOT_FOUND;
    const esp_err_t err = esp_ota_set_boot_partition(updater);
    if (err != ESP_OK)
        return err;
    esp_restart();
}

bool send_frame(const uint8_t data[APP_CONTROL_FRAME_BYTES])
{
    twai_message_t msg{};
    msg.identifier = update_reply_id;
    msg.data_length_code = APP_CONTROL_FRAME_BYTES;
    memcpy(msg.data, data, APP_CONTROL_FRAME_BYTES);
    return twai_transmit(&msg, pdMS_TO_TICKS(settings.reply_tx_wait_ms)) == ESP_OK;
}

// The host retries on a lost reply, so a refused frame is only logged.
void send_reply(uint8_t cmd, uint8_t err)
{
    uint8_t reply[APP_CONTROL_FRAME_BYTES];
    app_control_encode_reply(cmd, err, reply);
    if (!send_frame(reply))
        Serial.printf("can_update: reply to 0x%02X not sent (TX queue full)\n", (unsigned)cmd);
}

void serve_app_info()
{
    uint8_t frames[APP_CONTROL_INFO_BYTES];
    app_control_encode_info(firmware_version, firmware_label, frames);
    // A refused frame ends the burst: the host rejects a partial reply and asks again.
    for (size_t offset = 0; offset < sizeof(frames); offset += APP_CONTROL_FRAME_BYTES)
    {
        if (!send_frame(&frames[offset]))
        {
            Serial.printf("can_update: APP_INFO reply cut at frame %u (TX queue full)\n",
                          (unsigned)(offset / APP_CONTROL_FRAME_BYTES));
            return;
        }
    }
    Serial.println("can_update: APP_INFO answered");
}

void serve_enter_updater()
{
    if (esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, nullptr) == nullptr)
    {
        Serial.println("can_update: ENTER_UPDATER refused: no factory (updater) partition");
        send_reply(CAN_UPDATE_CMD_ENTER_UPDATER, CAN_UPDATE_ERR_FLASH);
        return;
    }

    Serial.println("can_update: ENTER_UPDATER acknowledged; restarting into the CAN updater");
    send_reply(CAN_UPDATE_CMD_ENTER_UPDATER, CAN_UPDATE_OK);
    // Let the ACK reach the wire before the restart resets TWAI.
    vTaskDelay(pdMS_TO_TICKS(settings.ack_drain_ms));
    const esp_err_t err = enter_updater();
    Serial.printf("can_update: cannot enter the CAN updater (%s); app keeps running\n", esp_err_to_name(err));
}

void update_task(void *)
{
    const TickType_t started = xTaskGetTickCount();
    const TickType_t stable_after = pdMS_TO_TICKS(settings.stable_uptime_ms);
    bool boot_counter_handled = false;

    for (;;)
    {
        TickType_t wait = portMAX_DELAY;
        if (!boot_counter_handled)
        {
            const TickType_t elapsed = xTaskGetTickCount() - started;
            wait = elapsed >= stable_after ? 0 : stable_after - elapsed;
        }

        uint32_t requests = 0;
        if (xTaskNotifyWait(0, kAllRequests, &requests, wait) == pdFALSE)
        {
            boot_counter_handled = true;
            clear_boot_counter();
            continue;
        }

        if ((requests & kRequestMalformed) != 0)
            send_reply(malformed_cmd.load(), CAN_UPDATE_ERR_STATE);
        if ((requests & kRequestAppInfo) != 0)
            serve_app_info();
        // Last: on success it does not return.
        if ((requests & kRequestEnterUpdater) != 0)
            serve_enter_updater();
    }
}

void request(uint32_t what)
{
    if (update_task_handle == nullptr)
        return;
    // eSetBits always succeeds for an existing task; requests intentionally coalesce.
    (void)xTaskNotify(update_task_handle, what, eSetBits);
}

}  // namespace

esp_err_t begin(const Config &config)
{
    if (update_task_handle != nullptr)
        return ESP_ERR_INVALID_STATE;
    if (config.task_core < 0 || config.task_core >= portNUM_PROCESSORS ||
        config.task_priority >= configMAX_PRIORITIES ||
        config.task_stack_size_bytes < configMINIMAL_STACK_SIZE ||
        config.task_stack_size_bytes % sizeof(StackType_t) != 0 ||
        (config.task_stack == nullptr && config.task_stack_size_bytes > sizeof(update_task_stack)) ||
        config.reply_tx_wait_ms == 0 || pdMS_TO_TICKS(config.reply_tx_wait_ms) == 0 ||
        pdMS_TO_TICKS(config.reply_tx_wait_ms) == portMAX_DELAY)
    {
        return ESP_ERR_INVALID_ARG;
    }

    const esp_err_t err = load_config();
    if (err != ESP_OK)
        return err;
    settings = config;
    load_firmware_identity();
    log_running_image();
    update_task_handle = xTaskCreateStaticPinnedToCore(
        update_task, "can_update", settings.task_stack_size_bytes, nullptr, settings.task_priority,
        settings.task_stack != nullptr ? settings.task_stack : update_task_stack,
        &update_task_storage, settings.task_core);
    if (update_task_handle == nullptr)
    {
        Serial.println("can_update: update task not created; no boot-counter clear or ENTER_UPDATER");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void on_standard_frame(uint32_t id, uint8_t dlc, const uint8_t data[8])
{
    if (id != update_cmd_id || data == nullptr)
        return;

    switch (app_control_parse_request(dlc, data))
    {
        case APP_CONTROL_REQ_APP_INFO:
            request(kRequestAppInfo);
            break;
        case APP_CONTROL_REQ_ENTER_UPDATER:
            request(kRequestEnterUpdater);
            break;
        case APP_CONTROL_REQ_MALFORMED:
            malformed_cmd.store(data[0]);
            request(kRequestMalformed);
            break;
        default:
            // Updater-only commands must stay silent so the host tries ENTER_UPDATER.
            break;
    }
}

uint16_t command_id()
{
    return update_cmd_id;
}

uint16_t reply_id()
{
    return update_reply_id;
}

void confirm_when_healthy(bool healthy)
{
    static bool confirmed = false;
    if (update_task_handle == nullptr || !settings.owns_image_confirmation || confirmed || !healthy)
        return;
    confirmed = true;

#ifdef CAN_UPDATE_SKIP_MARK_VALID
    Serial.println("can_update: self-test passed; image NOT marked valid (CAN_UPDATE_SKIP_MARK_VALID): "
                   "a pending image rolls back on the next reset");
#else
    const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err != ESP_OK)
    {
        Serial.printf("can_update: marking the image valid failed (%s)\n", esp_err_to_name(err));
        return;
    }
    Serial.println("can_update: self-test passed; image marked valid");
#endif
}

}  // namespace arduino_node_app
