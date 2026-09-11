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
    uint32_t   serial;     /* bumped on every grant; never 0 once anything was granted */
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
    /* A new identity for every grant, so a release aimed at one grant cannot land on a later
       one from the same source — link.c's queued releases are keyed on it. Skips 0 on wrap so
       that 0 can keep meaning "no grant" there. */
    a->serial = (a->serial + 1u == 0u) ? 1u : a->serial + 1u;
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
/* Two breadcrumb segments (recovery.h's RECOVER_SEG_MAX_MS = 250) plus a tick. Spelled here
 * rather than included from recovery.h: link.h is what recovery.h depends on, not the other
 * way round.
 *
 * RECOVER used to be sticky with no hold at all, which made the retreat the one streaming
 * source in the system with no time bound — a replay starved between a step and its release,
 * or a release that lost both lock races, left the last REVERSED command standing as the
 * actuator target with nothing to fall it to zero. RT and CALIB are both bounded by their
 * lapse; this is the retreat's.
 *
 * TWO segments and not one-plus-a-tick, which is what this was first set to (270). The
 * retreat's wait between steps is tick-aligned and wakes at exactly the segment's length, so
 * the next car_drive(RECOVER) lands at 250 ms + whatever the scheduler adds — and the retreat
 * task shares priority 5 with link_task's I2C pass and httpd's flash writes. One tick of slack
 * lapsed the grant on any delay of 20 ms, zeroed the target, and made the next step re-ramp
 * from nothing: most of the segment's distance lost, on every segment at the cap, which the
 * first segment of every retreat is. The hold only exists to bound a STARVED replay, so
 * doubling it costs nothing on a healthy one. */
#define LINK_HOLD_RECOVER_MS (2u * 250u + LINK_TICK_MS)

/* The shadow's "I do not know what the chip holds" value: at boot, and after a write the
 * driver reported as failed (which may or may not have landed — see link.c). Nonzero, so
 * link_rise_safe counts an unknown pair-mate as driving; unequal to every real duty, so a
 * channel in this state is always planned again. */
#define LINK_SHADOW_UNKNOWN 0xFFFFu

/* Pure: plan one actuator tick. next[] receives every channel's post-ramp duty;
 * order[] receives the channels that need writing — every falling channel first, then
 * the rises — and the count is returned. Channel pairs are (0,1)(2,3)(4,5)(6,7), one
 * BTS7960 each (motors.h): a single ascending pass wrote a reversal's rise before its
 * pair-mate's fall, driving both bridge inputs for the I2C gap between them.
 *
 * An unknown shadow ramps FROM ZERO, not from 0xFFFF. ramp_step treats any step down as
 * instant, and 0xFFFF is above every target — so ramping from the sentinel handed the retry
 * after a single failed write the full target in one tick (273 -> 4095 with no slew, verified
 * in a host model). From zero is the slowest rise the ramp allows, which is also the only
 * one that is safe when what the chip really holds is not known. The sentinel still counts
 * as "differs from cur", so the channel is ordered with the falls and always written. */
static inline uint8_t link_plan_writes(const uint16_t cur[8], const uint16_t tgt[8],
                                       uint16_t max_up, uint16_t next[8],
                                       uint8_t order[8]) {
    uint8_t n = 0;
    for (uint8_t ch = 0; ch < 8; ch++) {
        uint16_t from = (cur[ch] == LINK_SHADOW_UNKNOWN) ? 0 : cur[ch];
        next[ch] = ramp_step(from, tgt[ch], max_up);
        if (next[ch] < cur[ch]) order[n++] = ch;
    }
    for (uint8_t ch = 0; ch < 8; ch++) {
        if (next[ch] > cur[ch]) order[n++] = ch;
    }
    return n;
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

/* link_release, insisted upon: one retry a tick later, then the release is QUEUED for the
 * 50 Hz actuator task, which drains it under the lock it takes every pass. A false return
 * therefore means "not yet, but within a tick" — not "stuck".
 *
 * It used to mean stuck, and this comment used to forbid dropping the result because of it: a
 * sticky top-rank grant (SAFE after a goodbye, OTA on a failure path) left standing is an
 * actuator nothing of lower rank can ever take, and rt_glue_bye deliberately will not grab
 * SAFE over an OTA owner. Thirteen of fifteen callers dropped it anyway. Requiring fifteen
 * call sites to handle a failure the arbiter can finish itself was the wrong shape; the queue
 * is the fix, and a caller may now use the return for reporting or ignore it. */
bool link_release_must(link_src_t src);

/* Who owns the actuator right now, for telemetry and logs. */
link_src_t link_owner(void);

/* False once a PCA9685 write has failed and not yet succeeded again. */
bool link_bus_ok(void);
#endif /* LINK_HOST_TEST */

#endif /* LINK_H */
