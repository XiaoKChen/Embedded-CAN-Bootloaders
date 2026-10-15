/* =============================================================================
 * node_reset.c — the brain's reset line to the node
 * =============================================================================
 * See node_reset.h for the public interface.
 * ============================================================================= */

#include "node_reset.h"

#include "driver/gpio.h"
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
