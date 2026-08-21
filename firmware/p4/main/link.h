#ifndef LINK_H
#define LINK_H

#include <stdint.h>
#include <stdbool.h>

/* Who may command the actuator.
 *
 * The numeric order IS the priority, ascending: a request is granted when its source
 * ranks at or above the current owner. Each neighbouring pair is a decision:
 *
 *   RT above RECOVER   — when the driver comes back, the next control frame takes the
 *                        wheels back immediately. If the retreat outranked it, control
 *                        would be refused while a frame was arriving.
 *   CONSOLE above RECOVER — a bench command is not silently killed by a retreat. The
 *                        retreat can only run after real /ws traffic armed the watchdog,
 *                        so this costs nothing in a console-only session.
 *   RT above CONSOLE   — a live pilot outranks a command typed minutes ago.
 *   CALIB above RT     — the wizard's spin pulse must not be overwritten ~100 ms later
 *                        by the app's own 10 Hz zero-stream.
 *   OTA, SAFE on top   — flashing and an explicit stop answer to nobody.
 */
typedef enum {
    LINK_SRC_NONE    = -1,
    LINK_SRC_RECOVER = 0,
    LINK_SRC_CONSOLE,
    LINK_SRC_RT,
    LINK_SRC_CALIB,
    LINK_SRC_OTA,
    LINK_SRC_SAFE,
} link_src_t;

/* Pure arbitration state. Owned by link.c; exposed here so it can be host-tested. */
typedef struct {
    link_src_t owner;
    uint32_t   until_ms;   /* when a non-sticky grant expires */
    bool       sticky;     /* ownership ends only on release, never on time */
} link_arb_t;

/* Pure: is the actuator free at `now`?
 * Unsigned subtraction cast to signed, so the 32-bit millisecond counter's rollover
 * is handled the same way watchdog_stale and recovery_evict handle it. */
static inline bool link_arb_lapsed(const link_arb_t *a, uint32_t now) {
    if (a->owner == LINK_SRC_NONE) return true;
    if (a->sticky) return false;
    return (int32_t)(now - a->until_ms) >= 0;
}

/* Pure: may `src` command the actuator at `now`? Records the grant on success.
 * A refused request leaves the current grant exactly as it was. */
static inline bool link_arb_grant(link_arb_t *a, link_src_t src, uint32_t now,
                                  uint32_t hold_ms, bool sticky) {
    if (!link_arb_lapsed(a, now) && src < a->owner) return false;
    a->owner    = src;
    a->until_ms = now + hold_ms;
    a->sticky   = sticky;
    return true;
}

/* Pure: give up ownership, but only if `src` is the one holding it — a late release
 * must not steal the actuator from whoever has since taken over. */
static inline void link_arb_release(link_arb_t *a, link_src_t src) {
    if (a->owner != src) return;
    a->owner    = LINK_SRC_NONE;
    a->until_ms = 0;
    a->sticky   = false;
}

/* Pure: the name telemetry reports in "ctl", and logs use. */
static inline const char *link_src_name(link_src_t s) {
    switch (s) {
        case LINK_SRC_NONE:    return "none";
        case LINK_SRC_RECOVER: return "recover";
        case LINK_SRC_CONSOLE: return "console";
        case LINK_SRC_RT:      return "rt";
        case LINK_SRC_CALIB:   return "calib";
        case LINK_SRC_OTA:     return "ota";
        case LINK_SRC_SAFE:    return "safe";
        default:               return "?";
    }
}

#ifndef LINK_HOST_TEST
#include "esp_err.h"

/* How long each source's grant holds without being refreshed. */
#define LINK_HOLD_RT_MS     300u   /* the watchdog deadline; the 10 Hz stream refreshes it */
#define LINK_HOLD_CALIB_MS  600u   /* one identification pulse */

/* Start the 50 Hz actuator task. The sole writer to the PCA9685 after this call. */
esp_err_t link_init(void);

/* Ask to set the eight duties. Returns false when a higher-priority source holds the
 * actuator, in which case nothing is written and the caller must not treat the command
 * as applied — the control watchdog is fed only on a true return. */
bool link_set(link_src_t src, const uint16_t duty[8], uint32_t hold_ms, bool sticky);

/* Give up ownership held by `src`. Harmless if `src` does not hold it. */
void link_release(link_src_t src);

/* Who owns the actuator right now, for telemetry and logs. */
link_src_t link_owner(void);

/* False once a PCA9685 write has failed and not yet succeeded again. */
bool link_bus_ok(void);
#endif /* LINK_HOST_TEST */

#endif /* LINK_H */
