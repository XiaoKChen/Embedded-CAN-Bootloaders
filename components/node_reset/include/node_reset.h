/* =============================================================================
 * node_reset.h — the brain's reset line to the node
 * =============================================================================
 * The brain drives the node's EN (reset) from CONFIG_NODE_RESET_GPIO (GPIO3)
 * through an inverting stage: HIGH holds the node in reset, LOW lets it run.
 * Shared by every firmware that runs on the brain (brain/, the SLCAN bridge).
 * ============================================================================= */

#ifndef NODE_RESET_H
#define NODE_RESET_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif


/* -----------------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------------- */

/**
 * Drive the reset line LOW so the node runs, and keep driving it. Call it
 * first thing at boot: until then the pin floats, which can hold the node in
 * reset.
 *
 * @return ESP_OK on success, or the GPIO driver's error.
 */
esp_err_t node_reset_release(void);


#ifdef __cplusplus
}
#endif

#endif /* NODE_RESET_H */
