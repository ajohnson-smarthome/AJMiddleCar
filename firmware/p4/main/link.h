#ifndef LINK_H
#define LINK_H

#include <stdint.h>
#include <stdbool.h>
#include "contract.h"   /* RT_WATCHDOG_MS, RT_COMMAND_HZ, the CTL_* vocabulary */
#include "ramp.h"

/* Who may command the actuator.
 *
 * The numeric order IS the priority, ascending: a request is granted when its source
 * ranks at or above the current owner. Each neighbouring pair is a decision:
 *
 *   RT above RECOVER   — when the driver comes back, the next control frame takes the
 *                        wheels back immediately. If the retreat outranked it, control
 *                        would be refused while a frame was arriving.
 *   CONSOLE above RECOVER — a bench command is not silently killed by a retreat. The
 *                        retreat can only run after real traffic on the real-time
 *                        channel armed the watchdog, so this costs nothing in a
 *                        console-only session.
 *   RT above CONSOLE   — a live pilot outranks a command typed minutes ago.
 *   CALIB above RT     — the wizard's spin pulse must not be overwritten one
 *                        RT_COMMAND_HZ period later by the app's own zero-stream.
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

/* Pure: the name telemetry reports in "ctl", and logs use. The spellings are the
 * schema's ctl_values, so the app and the mock read the same words this returns. */
static inline const char *link_src_name(link_src_t s) {
    switch (s) {
        case LINK_SRC_NONE:    return CTL_NONE;
        case LINK_SRC_RECOVER: return CTL_RECOVER;
        case LINK_SRC_CONSOLE: return CTL_CONSOLE;
        case LINK_SRC_RT:      return CTL_RT;
        case LINK_SRC_CALIB:   return CTL_CALIB;
        case LINK_SRC_OTA:     return CTL_OTA;
        case LINK_SRC_SAFE:    return CTL_SAFE;
        default:               return "?";
    }
}

/* One enumerator per word in the schema's ctl_values (LINK_SRC_NONE included): a value
 * added to the contract must break this build rather than surface as "?" on the wire. */
_Static_assert(LINK_SRC_SAFE + 2 == CTL_COUNT, "link_src_t and ctl_values disagree");

/* The actuator task's beat, public because the RT hold is defined against it. */
#define LINK_TICK_MS 20u

/* How long each source's grant holds without being refreshed. The RT hold is one
 * actuator tick PAST the control watchdog's deadline, and necessarily so: the trip
 * must be declared before the grant lapses, or the car coasts to a stop before the
 * loss is noticed and the retreat starts from rest instead of from the path it was
 * on. rt_link checks silence on a beat of its own, so one tick of slack covers the
 * scheduling gap; the RT_COMMAND_HZ stream refreshes the grant far inside it. */
#define LINK_HOLD_RT_MS     ((uint32_t)RT_WATCHDOG_MS + LINK_TICK_MS)
#define LINK_HOLD_CALIB_MS  600u   /* one identification pulse */

/* Pure: plan one actuator tick. next[] receives every channel's post-ramp duty;
 * order[] receives the channels that need writing — every falling channel first, then
 * the rises — and the count is returned. Channel pairs are (0,1)(2,3)(4,5)(6,7), one
 * BTS7960 each (motors.h): a single ascending pass wrote a reversal's rise before its
 * pair-mate's fall, driving both bridge inputs for the I2C gap between them.
 * The rise bound is per channel because a kicked channel bypasses the ramp for its
 * kick ticks while its neighbours keep theirs (link_kick_plan). */
static inline uint8_t link_plan_writes(const uint16_t cur[8], const uint16_t tgt[8],
                                       const uint16_t up[8], uint16_t next[8],
                                       uint8_t order[8]) {
    uint8_t n = 0;
    for (uint8_t ch = 0; ch < 8; ch++) {
        next[ch] = ramp_step(cur[ch], tgt[ch], up[ch]);
        if (next[ch] < cur[ch]) order[n++] = ch;
    }
    for (uint8_t ch = 0; ch < 8; ch++) {
        if (next[ch] > cur[ch]) order[n++] = ch;
    }
    return n;
}

/* Pure: start-assist for stiction. A channel leaving standstill (cur==0, tgt>0)
   on a command below kick_duty gets kick_ticks ticks at kick_duty with the ramp
   bypassed for those ticks. tgt==0 disarms instantly: a stop is never kicked,
   the instant fall stays instant. */
static inline void link_kick_plan(const uint16_t cur[8], uint16_t tgt[8],
                                  uint16_t up[8], uint8_t kick[8],
                                  uint16_t ramp_up, uint16_t kick_duty,
                                  uint8_t kick_ticks) {
    for (uint8_t ch = 0; ch < 8; ch++) {
        up[ch] = ramp_up;
        if (tgt[ch] == 0) { kick[ch] = 0; continue; }
        if (cur[ch] == 0 && kick[ch] == 0 && tgt[ch] < kick_duty) kick[ch] = kick_ticks;
        if (kick[ch]) { kick[ch]--; if (tgt[ch] < kick_duty) tgt[ch] = kick_duty; up[ch] = 4095; }
    }
}

/* Pure: may a channel be driven to `duty` while its pair-mate's last-written duty is
 * `mate_cur`? Writing zero is always safe; a nonzero rise needs the mate at zero on
 * the chip. The boot shadow's unknown value is nonzero, so it counts as driving. */
static inline bool link_rise_safe(uint16_t mate_cur, uint16_t duty) {
    return duty == 0 || mate_cur == 0;
}

#ifndef LINK_HOST_TEST
#include "esp_err.h"

/* Start the 50 Hz actuator task. The sole writer to the PCA9685 after this call. */
esp_err_t link_init(void);

/* Ask to set the eight duties. Returns false when a higher-priority source holds the
 * actuator: nothing was written, and the caller must not treat the command as applied.
 * (Only the breadcrumb recorder keys on this return; the control watchdog is fed
 * upstream on every parsed in-session command, refused ones included — see car.h.) */
bool link_set(link_src_t src, const uint16_t duty[8], uint32_t hold_ms, bool sticky);

/* Give up ownership held by `src`. Harmless if `src` does not hold it. Returns false
 * only when the lock could not be taken — the caller still does not own the actuator,
 * but the safe target was not written either, so a safety caller should say so. */
bool link_release(link_src_t src);

/* link_release, insisted upon: one retry a tick later, then a loud log. A false
 * return leaves `src`'s grant standing — for a sticky top-rank source (SAFE after a
 * goodbye) that is an actuator nothing else can ever take, so no caller may drop the
 * result on the floor. */
bool link_release_must(link_src_t src);

/* Who owns the actuator right now, for telemetry and logs. */
link_src_t link_owner(void);

/* False once a PCA9685 write has failed and not yet succeeded again. */
bool link_bus_ok(void);
#endif /* LINK_HOST_TEST */

#endif /* LINK_H */
