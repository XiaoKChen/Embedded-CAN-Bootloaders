/**
 * @file    package_update.h
 * @brief   Brain + nodes update packages: staging, validation, brain update and node relay.
 *
 * Serves POST /package and GET /package/status: stages a v1 package into the pkg partition, checks
 * it, updates the brain from it when its image differs, then relays each node payload over CAN.
 *
 * Owns:         The pkg partition, the pending-relay record in NVS and the package status.
 * Not here:     Manifest parsing (src/boot/update_pkg.h); node flashing and recovery
 *               (node_flash.h, node_reset.h); the HTTP worker (http_ota_server.c).
 * Invariants:   The pending record is committed before boot selection or the first CAN mutation,
 *               so a brain update that rolls back aborts the node relay. A retained record that
 *               cannot be safely cleared blocks new mutating jobs.
 * Concurrency:  Every call but status/blocked runs exclusively on the HTTP worker.
 * Errors:       Handlers answer with HTTP statuses; the job outcome is reported by
 *               package_update_status().
 *
 * @see     package_update.c, docs/reference/package-format.md, docs/testing/bench/update-package.md
 */

#ifndef PACKAGE_UPDATE_H
#define PACKAGE_UPDATE_H

#include <stdbool.h>
#include "esp_err.h"
#include "esp_http_server.h"

/* All operations except status/blocked run exclusively on the HTTP worker.
 * resume is called once after the startup self-test; it initializes storage.
 * A retained record that cannot be safely cleared blocks new mutating jobs. */
void package_update_resume(void);
bool package_update_blocked(void);
esp_err_t package_update_stage(httpd_req_t *req, bool *ready);
void package_update_run(void);
esp_err_t package_update_status(httpd_req_t *req);

#endif
