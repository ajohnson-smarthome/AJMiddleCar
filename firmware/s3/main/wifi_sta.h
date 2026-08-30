#ifndef WIFI_STA_H
#define WIFI_STA_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "net_cfg.h"

/* The radio, as a station. Holds esp_wifi and no policy: the retry budget and the give-up rule
 * live in wifi_state.{c,h}, which is pure and host-tested.
 *
 * The dongle learns nothing about the car here. It is handed an opaque SSID and password by
 * net_api and joins whatever answers to them; where to forward traffic afterwards comes from the
 * gateway DHCP gave us, because a softAP's gateway is the softAP.
 *
 * No connected-callback: Tasks 4 and 5 both need to know when the station (re)connects, and one
 * callback slot cannot serve two registrants without one silently overwriting the other. Both
 * relays already run a select() loop that wakes at least once a second, so they poll
 * wifi_sta_gateway() instead of being told. */

/* Bring up the station. Joins immediately if a configuration is already stored. Safe to call
 * once, from app_main, after nvs_flash_init and esp_event_loop_create_default. */
esp_err_t wifi_sta_start(void);

/* Join this network, restarting the attempt budget. net_api calls this when a POST /net changed
 * the stored value — an unchanged POST must not restart a working radio. */
void wifi_sta_join(const net_cfg_t *cfg);

/* GET /status's `net.state`, spelled by the generated contract. */
const char *wifi_sta_state_name(void);

/* GET /status's `net.rssi`. 0 when not connected — the dongle is a station and reads its own
 * receiver, so unlike the car this is a real measurement whenever it is non-zero. */
int8_t wifi_sta_rssi(void);

/* The gateway of the joined network, in network byte order. False until the FIRST address
 * ever arrives; once true, it stays true and keeps the last-known gateway even across a
 * drop and a retry — it is not cleared on disconnect, because a softAP's gateway does not
 * move between joins of the same network. This is not a liveness signal: call
 * wifi_sta_state_name() for whether the radio is actually connected right now. This is the
 * relays' destination, and the only reason the dongle needs no compiled-in car address. */
bool wifi_sta_gateway(uint32_t *out_be);

#endif /* WIFI_STA_H */
