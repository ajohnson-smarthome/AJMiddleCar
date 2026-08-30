#ifndef STATUS_API_H
#define STATUS_API_H

#include "esp_err.h"

/* Starts the HTTP server and registers GET /status.
 *
 * Bound to the USB interface only. The dongle's configuration must never be
 * reachable over the radio — from Plan 2 this endpoint carries a car's password. */
esp_err_t status_api_start(void);

#endif /* STATUS_API_H */
