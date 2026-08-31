#ifndef OTA_API_H
#define OTA_API_H

#include "esp_err.h"
#include "esp_http_server.h"

/* Registers POST /ota on an already-running server — streams an app image into the inactive
 * slot, validates it, and reboots into it. Takes the handle rather than starting a server of
 * its own, the way net_api_register does: status_api owns the one server this firmware has.
 *
 * Same situation as status_api.h: this server is not bound to the USB interface — IDF 6.0.2's
 * httpd_config_t has no bind-address field, so it listens on INADDR_ANY, reachable from the
 * car's network alongside USB. /ota is unauthenticated, which is a worse thing to expose than
 * a password, and what makes it safe is status_api_start's open_fn (api_guard.c): every
 * accepted connection that did not land on DONGLE_HOST is refused before a byte of this
 * handler's request is ever read. */
esp_err_t ota_api_register(httpd_handle_t server);

#endif /* OTA_API_H */
