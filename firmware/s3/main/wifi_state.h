#ifndef WIFI_STATE_H
#define WIFI_STATE_H

#include <stdbool.h>
#include <stdint.h>

#include "dongle_contract.inc"

/* The join, as a policy separate from the radio that enacts it.
 *
 * Pure: no ESP-IDF, so the retry budget and the give-up rule are host-tested rather than
 * reasoned about on a bench with a car that may or may not be switched on. wifi_sta.c holds
 * the esp_wifi calls and no policy of its own.
 *
 * The rule this exists to enforce: a join that fails gives up and stays given up. A radio that
 * hunts for an absent car indefinitely drains the phone it is plugged into, and the phone
 * cannot see it happening. The app restarts the attempt by POSTing /net again. */

typedef enum {
    WIFI_IDLE = 0,   /* nothing configured yet — the dongle has never been told a network */
    WIFI_JOINING,
    WIFI_CONNECTED,  /* associated AND addressed: the relays have a gateway to aim at */
    WIFI_FAILED,     /* the budget ran out; held until a new configuration arrives */
} wifi_state_t;

typedef enum {
    WIFI_EV_CONFIGURED,    /* a POST /net arrived with a network to join */
    WIFI_EV_DISCONNECTED,  /* association lost, or an attempt failed */
    WIFI_EV_GOT_IP,        /* DHCP completed — the only event that means "usable" */
} wifi_ev_t;

/* Five attempts is a judgement, not a measurement: enough to ride out a car still booting its
 * softAP, few enough that a car which is simply off costs seconds rather than a battery. */
#define WIFI_JOIN_ATTEMPTS 5

typedef struct {
    wifi_state_t state;
    uint8_t      attempts;  /* consumed attempts in the current budget */
} wifi_sm_t;

void wifi_state_init(wifi_sm_t *sm);

/* Feed one event. Returns true when the caller should attempt a connection now — the module
 * never calls the radio itself, which is what keeps it pure and testable. */
bool wifi_state_step(wifi_sm_t *sm, wifi_ev_t ev);

/* The state as GET /status spells it, straight from the generated contract. */
const char *wifi_state_name(const wifi_sm_t *sm);

#endif /* WIFI_STATE_H */
