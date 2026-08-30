#ifndef OTA_API_H
#define OTA_API_H

#include "esp_err.h"
#include "esp_http_server.h"

/* Registers POST /ota on an already-running server — streams an app image into the inactive
 * slot, validates it, and reboots into it. Takes the handle rather than starting a server of
 * its own, the way net_api_register does: status_api owns the one server this firmware has.
 *
 * Same warning as status_api.h: this server is not bound to the USB interface. When Plan 4
 * links a radio, /ota becomes reachable from the car's network alongside POST /net — and an
 * unauthenticated firmware-write endpoint is a worse thing to expose than a password. Whoever
 * brings up the station brings the peer check first. */
esp_err_t ota_api_register(httpd_handle_t server);

#endif /* OTA_API_H */
