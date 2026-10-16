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
