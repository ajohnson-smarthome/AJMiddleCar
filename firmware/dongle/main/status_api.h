#ifndef STATUS_API_H
#define STATUS_API_H

#include <stdbool.h>

#include "esp_err.h"
#include "esp_http_server.h"

/* Starts the HTTP server and registers GET /status.
 *
 * NOT bound to the USB interface — httpd_start listens on INADDR_ANY, and IDF's
 * esp_http_server offers no interface-binding option (httpd_config_t has server_port
 * and ctrl_port, nothing address-shaped). Since the station came up this surface is
 * reachable from the car's network as well as USB, and this server carries a car's
 * password (POST /net) and an unauthenticated firmware-write endpoint (POST /ota).
 * What makes that safe: status_api_start sets httpd_config_t.open_fn to
 * api_guard_open (api_guard.h), which refuses every accepted connection that did not
 * land on DONGLE_HOST, before a single request byte is read. */
esp_err_t status_api_start(void);

/* The running server, so another module can register its handlers on it rather than
 * start a second one. NULL before status_api_start succeeds. */
httpd_handle_t status_api_server(void);

/* Did the bootloader revert the previous OTA? The same answer GET /status publishes as
 * `rollback`, read once at status_api_start and unable to change without a reboot.
 *
 * Exposed rather than duplicated: the display needs it too, and a second module querying the
 * partition table itself would be two independent readings of one unchanging fact, free to
 * disagree if either ever grew a condition. False before status_api_start has run. */
bool status_api_rolled_back(void);

#endif /* STATUS_API_H */
