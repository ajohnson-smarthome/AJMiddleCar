#include "wifi_state.h"

void wifi_state_init(wifi_sm_t *sm)
{
    sm->state = WIFI_IDLE;
    sm->attempts = 0;
}

bool wifi_state_step(wifi_sm_t *sm, wifi_ev_t ev)
{
    switch (ev) {
    case WIFI_EV_CONFIGURED:
        /* From any state, including FAILED: a new configuration is the app saying "try again",
           and it restores the whole budget rather than the remainder of a spent one. */
        sm->state = WIFI_JOINING;
        sm->attempts = 0;
        return true;

    case WIFI_EV_GOT_IP:
        /* An address is the only evidence that matters: association alone leaves the relays
           with no gateway to aim at. Idempotent, so a DHCP renewal changes nothing. */
        sm->state = WIFI_CONNECTED;
        sm->attempts = 0;
        return false;

    case WIFI_EV_DISCONNECTED:
        if (sm->state == WIFI_IDLE || sm->state == WIFI_FAILED) {
            return false;  /* nothing configured, or already given up — do not retry */
        }
        if (sm->state == WIFI_CONNECTED) {
            /* A link that worked once and dropped gets a fresh budget: a car switched off and
               on again is the ordinary case, not an escalating failure. */
            sm->state = WIFI_JOINING;
            sm->attempts = 1;
            return true;
        }
        sm->attempts++;
        if (sm->attempts >= WIFI_JOIN_ATTEMPTS) {
            sm->state = WIFI_FAILED;
            return false;
        }
        return true;
    }
    return false;  /* unreachable for the enum above; keeps -Werror quiet about the return */
}

const char *wifi_state_name(const wifi_sm_t *sm)
{
    switch (sm->state) {
    case WIFI_JOINING:   return DONGLE_STATE_JOINING;
    case WIFI_CONNECTED: return DONGLE_STATE_CONNECTED;
    case WIFI_FAILED:    return DONGLE_STATE_FAILED;
    case WIFI_IDLE:      break;
    }
    return DONGLE_STATE_IDLE;
}
