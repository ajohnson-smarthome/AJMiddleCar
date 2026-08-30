#ifndef NET_API_H
#define NET_API_H

#include <stdbool.h>
#include "esp_err.h"
#include "esp_http_server.h"
#include "net_cfg.h"

/* GET /net  → {"ssid":"…","configured":true|false}   — never the password
 * POST /net ← {"ssid":"…","password":"…"}            → {"ok":true}
 *
 * The SSID is opaque here. This firmware does not know what a car is. */

/* Load the stored configuration from NVS. Call once at boot, before registering. */
void net_api_load(void);

/* Register both handlers on an already-running server. */
esp_err_t net_api_register(httpd_handle_t server);

/* The live configuration. Returns false when none has been set, in which case *out is
 * left untouched. Plan 3's radio reads this to know what to join. */
bool net_api_current(net_cfg_t *out);

#endif /* NET_API_H */
