/**
 * @file    arduino_node_app.hpp
 * @brief   Stock Arduino-ESP32 node-app support for the factory CAN updater.
 *
 * Loads provisioned CAN IDs and board-profile LOW holds, answers APP_INFO and ENTER_UPDATER,
 * and clears the flash boot counter after stable uptime. Uses the app's existing TWAI driver;
 * no custom RTC reservation or recovery.c is required on the stock Arduino core.
 *
 * Owns:         One statically allocated update task, its request notifications, the loaded ID
 *               pair, and optional one-shot image confirmation after the app's self-test.
 * Not here:     TWAI installation, filtering and RX (the app's transport); hardware self-test;
 *               RTC crash-loop counting and clean-restart marks; verifyRollbackLater().
 * Invariants:   Call begin() once, early in setup(), before registering the RX callback. Keep
 *               profile hold pins LOW and do not repurpose them. The app MUST define
 *               extern "C" bool verifyRollbackLater() { return true; } to defer Arduino's
 *               pre-setup image confirmation (retain an existing OTA owner's hook instead).
 *               Enable this component with custom_arduino_node_app = yes in PlatformIO.
 * Concurrency:  One task calls on_standard_frame() for standard DATA frames only, never an ISR.
 *               It only parses and posts notifications; all replies and updater entry run on
 *               the update task. Call confirm_when_healthy() from one app task after begin().
 * Errors:       begin() returns ESP_ERR_INVALID_ARG, ESP_ERR_INVALID_STATE, ESP_ERR_NO_MEM or
 *               a GPIO error. Missing/invalid cancfg records use protocol/profile defaults.
 *               Async flash/TX/OTA errors are logged; failed updater entry leaves the app alive.
 * Depends on:   Arduino-ESP32, ESP-IDF TWAI/OTA/partition/GPIO, FreeRTOS and the shared C codecs.
 *
 * @see     arduino_node_app.cpp, app_control.h, board_profile.h, boot_counter.h
 */
#ifndef ARDUINO_NODE_APP_HPP
#define ARDUINO_NODE_APP_HPP

#include <cstdint>
#include <boot_counter.h>
#include <esp_err.h>
#include <freertos/FreeRTOS.h>

namespace arduino_node_app {

constexpr BaseType_t kDefaultTaskCore = 0;
constexpr UBaseType_t kDefaultTaskPriority = 1;
constexpr uint32_t kDefaultTaskStackSizeBytes = 4096;
constexpr uint32_t kDefaultReplyTxWaitMs = 50;
constexpr uint32_t kDefaultAckDrainMs = 20;

struct Config {
    // nullptr uses USB_PRODUCT split at its last space, just like the SOLO ODU
    // (empty when USB_PRODUCT is undefined). Overrides must remain valid and
    // unchanged for the lifetime of the helper.
    const char *label = nullptr;
    const char *version = nullptr;
    BaseType_t task_core = kDefaultTaskCore;
    UBaseType_t task_priority = kDefaultTaskPriority;
    uint32_t task_stack_size_bytes = kDefaultTaskStackSizeBytes;
    // nullptr uses the internal 4096-byte stack (size must not exceed it). To use a
    // larger stack, supply a static StackType_t array of task_stack_size_bytes bytes.
    StackType_t *task_stack = nullptr;
    uint32_t stable_uptime_ms = BOOT_COUNTER_STABLE_MS;
    uint32_t reply_tx_wait_ms = kDefaultReplyTxWaitMs;
    uint32_t ack_drain_ms = kDefaultAckDrainMs;
    // false leaves ALL marking-valid decisions to the app's own OTA self-test.
    bool owns_image_confirmation = true;
};

[[nodiscard]] esp_err_t begin(const Config &config = Config{});
void on_standard_frame(uint32_t id, uint8_t dlc, const uint8_t data[8]);
uint16_t command_id();
uint16_t reply_id();

// Once healthy, attempt confirmation once per boot, as on the ODU. With ownership
// disabled this is a no-op. CAN_UPDATE_SKIP_MARK_VALID preserves the rollback bench test.
void confirm_when_healthy(bool healthy);

}  // namespace arduino_node_app

#endif  // ARDUINO_NODE_APP_HPP
