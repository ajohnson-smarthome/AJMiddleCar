#ifndef STATUS_JSON_H
#define STATUS_JSON_H

#include <stdbool.h>
#include <stddef.h>

#include "dongle_contract.inc"

/* Everything /status says, as plain values, so the body can be rendered and tested on the
 * host with no ESP-IDF in sight. status_api.c fills this from the live modules in the
 * read order its own comments prescribe; this file only spells the JSON.
 *
 * Pure: no ESP-IDF, no cJSON. Compiled with plain cc in the test Makefile. */
typedef struct {
    const char *fw;                 /* esp_app_desc_t.version */
    const char *idf;
    bool        rolled_back;
    const char *usb_state;          /* DONGLE_USB_STATE_* */
    const char *ssid;               /* raw; escaped here */
    bool        configured;
    const char *wifi_state;         /* DONGLE_WIFI_STATE_* */
    bool        connected;          /* rssi_dbm and channel are readings only when true */
    int         rssi_dbm;
    unsigned    channel;
    unsigned    attempts_used;
    unsigned    attempts_max;
    unsigned    to_car_x10;         /* relay_stats keeps packets/s x10; the wire says 10.0 */
    unsigned    to_phone_x10;
    unsigned    udp_sessions;
    unsigned    tcp_connections;
    int         last_errno;         /* 0: never failed since boot -> last_error is null */
    const char *last_error_message; /* strerror(last_errno); ASCII, no quotes */
    unsigned    last_error_count;
    unsigned    last_error_age_s;
    long        uptime_s;
    unsigned    free_heap;
} status_view_t;

/* The whole GET /status body, proto first, groups in the contract's order. Returns the
 * length, or -1 when it does not fit — a truncated document parses as something else or
 * nothing, and the caller answers 500 rather than sending what fits. */
int status_json_render(const status_view_t *v, char *buf, size_t n);

#endif /* STATUS_JSON_H */
