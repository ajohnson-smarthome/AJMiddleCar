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


/* Register both handlers on an already-running server. */
esp_err_t net_api_register(httpd_handle_t server);

/* The live configuration. Returns false when none has been set since boot, in which case
 * *out is left untouched. Held in RAM only — see net_api.c for why nothing about the
 * network is allowed to survive a reboot.
 *
 * Unsynchronised, and read from two tasks since the panel arrived: this copies a file-static
 * that POST /net rewrites from the httpd task, and the display task calls it five times a
 * second to put the SSID on screen. Nothing guards the copy and nothing should — the
 * display's whole standing rule is that it never waits for anything, and a lock is a thing
 * to wait on. The cost of losing the race is one frame carrying the head of the old name and
 * the tail of the new, on the one occasion a person is repointing the dongle and watching
 * it do so; the next frame, 200 ms later, is correct. */
bool net_api_current(net_cfg_t *out);

#endif /* NET_API_H */
