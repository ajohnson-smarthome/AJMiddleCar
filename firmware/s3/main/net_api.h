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
 * left untouched. wifi_sta reads this at boot to know what to join.
 *
 * Unsynchronised, and read from two tasks since the panel arrived: this copies a file-static
 * that POST /net rewrites from the httpd task, and the display task calls it five times a
 * second to put the SSID on screen. Nothing guards the copy and nothing should — a lock here
 * would be one the display task could wait on while the httpd task is inside an NVS write, and
 * the display's whole standing rule is that it never waits for anything. The cost of losing
 * the race is one frame carrying the head of the old name and the tail of the new, on the one
 * occasion a person is repointing the dongle and watching it do so; the next frame, 200 ms
 * later, is correct. wifi_sta's own read is the boot one, before the server exists. */
bool net_api_current(net_cfg_t *out);

#endif /* NET_API_H */
