/**
 * @file    node_reset.c
 * @brief   The brain's reset line to the node (see node_reset.h for the contract).
 *
 * Implementation notes: recovery pulses HIGH to reset, LOW to run, and the final GPIO operation
 * releases the node.
 *
 * @see     node_reset.h
 */

#include "node_reset.h"

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"


/* -----------------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------------- */
/* GPIO3 (CONFIG_NODE_RESET_GPIO) drives the node's reset through an inverting
 * stage: HIGH holds the node in reset, LOW lets it run. A floating GPIO3 can
 * read as HIGH, so this firmware drives it LOW and never resets the node. The
 * level is latched before the output is enabled so the pin never glitches
 * HIGH. */
esp_err_t node_reset_release(void)
{
    const esp_err_t err = gpio_set_level(CONFIG_NODE_RESET_GPIO, 0);
    if (err != ESP_OK) {
        return err;
    }
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << CONFIG_NODE_RESET_GPIO,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    return gpio_config(&cfg);
}

esp_err_t node_reset_pulse_train(uint32_t pulses, uint32_t high_ms, uint32_t low_ms)
{
    esp_err_t err = node_reset_release();
    if (err == ESP_OK && (pulses == 0u || high_ms == 0u || low_ms == 0u)) {
        err = ESP_ERR_INVALID_ARG;
    }
    for (uint32_t i = 0u; i < pulses && err == ESP_OK; ++i) {
        err = gpio_set_level(CONFIG_NODE_RESET_GPIO, 1);
        if (err != ESP_OK) { break; }
        vTaskDelay(pdMS_TO_TICKS(high_ms));
        err = gpio_set_level(CONFIG_NODE_RESET_GPIO, 0);
        if (err != ESP_OK) { break; }
        vTaskDelay(pdMS_TO_TICKS(low_ms));
    }
    /* Retry a LOW latch even when a preceding GPIO call failed. */
    const esp_err_t release_err = node_reset_release();
    return err != ESP_OK ? err : release_err;
}
