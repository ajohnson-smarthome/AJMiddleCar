#ifndef DONGLE_CLOCK_H
#define DONGLE_CLOCK_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_timer.h"

/* The one millisecond clock, and the one log throttle.
 *
 * Before this header there were four private clocks on two time bases: relay_udp.c,
 * relay_tcp.c and api_guard.c each kept a now_ms() on the FreeRTOS tick, and relay_udp.c,
 * relay_tcp.c and wifi_sta.c each grew a second one on esp_timer for a field somebody else
 * would subtract from. The tick starts when the scheduler does, esp_timer at boot, so the two
 * differ by a constant nobody remembers — and a reader subtracting one from the other gets an
 * age wrong by exactly that. relay_stats.h had to carry a paragraph warning callers which
 * clock its fields were on. Now there is nothing to warn about: every millisecond field in
 * this firmware is boot_ms(), and any reader with boot_ms() may subtract from any of them.
 *
 * Ten hand-copied `static uint32_t last_log` blocks went with them. Nine started at 0, which
 * silently drops every throttled log in the first second after boot — the second in which a
 * refused connection or a failed first send is most worth reading. api_guard.c had found and
 * fixed that in its own copy, with a comment; the other nine never followed. A throttle that
 * is seeded correctly by its initialiser cannot be copied wrong. */

/* Milliseconds since boot, on esp_timer. Wraps after ~49.7 days; every consumer subtracts
 * two readings and never compares them, so the wrap costs nothing (see udp_sess_expire). */
static inline uint32_t boot_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* Passes once a second at most. Declared `static log_throttle_t t = LOG_THROTTLE_INIT;` at
 * the log site; the initialiser seeds it one interval in the past so the very first call
 * passes. */
typedef struct {
    uint32_t last_ms;
} log_throttle_t;

#define LOG_THROTTLE_MS   1000u
#define LOG_THROTTLE_INIT { .last_ms = (uint32_t)-(LOG_THROTTLE_MS + 1u) }

static inline bool log_throttle_ok(log_throttle_t *t, uint32_t now)
{
    if ((uint32_t)(now - t->last_ms) <= LOG_THROTTLE_MS) return false;
    t->last_ms = now;
    return true;
}

#endif /* DONGLE_CLOCK_H */
