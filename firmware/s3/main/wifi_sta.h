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

/* Join this network, restarting the attempt budget. Returns the first error that stopped the
 * request from reaching the radio (esp_wifi_set_config, esp_wifi_connect), or ESP_OK when the
 * attempt is genuinely under way — ESP_OK means "the join started", never "the join
 * succeeded"; poll wifi_sta_state_name() for the outcome.
 *
 * net_api calls this when a POST /net changed the stored value, and ALSO when an unchanged
 * POST arrives while the station is not connected. The rule is "an unchanged POST must not
 * restart a WORKING radio" — not "an unchanged POST does nothing". Those read the same until
 * the state is `failed`, which is the one state the retry exists for: the design says a failed
 * join is held rather than retried forever and the app decides when to try again by POSTing
 * again, so an unchanged re-POST is exactly how that decision arrives. */
esp_err_t wifi_sta_join(const net_cfg_t *cfg);

/* Whether the station is associated AND addressed right now. Lock-free, so /net's handler can
 * ask without waiting behind the event task; it reads the same _Atomic mirror
 * wifi_sta_state_name falls back to. This is a liveness question, not a configuration one —
 * net_api uses it to tell "the radio is already doing what you asked" from "the radio gave up
 * and you are asking again". */
bool wifi_sta_connected(void);

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
