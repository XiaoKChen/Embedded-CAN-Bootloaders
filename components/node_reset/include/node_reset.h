/**
 * @file    node_reset.h
 * @brief   The brain's reset line to the node.
 *
 * The brain drives the node's EN (reset) from CONFIG_NODE_RESET_GPIO (GPIO3) through an inverting
 * stage: HIGH holds the node in reset, LOW lets it run. Shared by every firmware that runs on the
 * brain (brain/, the SLCAN bridge).
 *
 * Owns:         The reset GPIO and the recovery pulse-train timing.
 * Invariants:   A pulse train forces the node into recovery and always finishes LOW.
 * Concurrency:  node_reset_pulse_train() blocks for the requested time: call it only from a task.
 * Errors:       Calls return esp_err_t: ESP_OK, an invalid argument or the GPIO driver's error.
 *
 * @see     node_reset.c, docs/concepts/recovery.md
 */

#ifndef NODE_RESET_H
#define NODE_RESET_H

#include <stdint.h>

#include "esp_err.h"

#define NODE_RESET_RECOVERY_PULSES  4u
#define NODE_RESET_RECOVERY_HIGH_MS 100u
#define NODE_RESET_RECOVERY_LOW_MS  400u

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

/**
 * Hold the node in reset (HIGH), then release it (LOW), pulses times.
 * The line is released on every exit, including an invalid argument or GPIO
 * error. Call only from a task; this operation blocks for the requested time.
 */
esp_err_t node_reset_pulse_train(uint32_t pulses, uint32_t high_ms, uint32_t low_ms);


#ifdef __cplusplus
}
#endif

#endif /* NODE_RESET_H */
