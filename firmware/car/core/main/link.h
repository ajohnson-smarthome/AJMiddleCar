#ifndef LINK_H
#define LINK_H

#include <stdint.h>
#include <stdbool.h>
#include "contract.h"   /* RT_WATCHDOG_MS, RT_COMMAND_HZ, the MOTORS_OWNER_* vocabulary */
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

/* Pure: the word telemetry reports in motors.owner, and logs use. The spellings are the
 * schema's, so the app and the mock read the same words this returns. */
static inline const char *link_src_name(link_src_t s) {
    switch (s) {
        case LINK_SRC_NONE:    return MOTORS_OWNER_IDLE;
        case LINK_SRC_RECOVER: return MOTORS_OWNER_RECOVERING;
        case LINK_SRC_CONSOLE: return MOTORS_OWNER_CONSOLE;
        case LINK_SRC_RT:      return MOTORS_OWNER_REMOTE;
        case LINK_SRC_CALIB:   return MOTORS_OWNER_CALIBRATION;
        case LINK_SRC_OTA:     return MOTORS_OWNER_UPDATE;
        case LINK_SRC_SAFE:    return MOTORS_OWNER_SAFE_STOP;
        default:               return "?";
    }
}

/* One enumerator per word in the schema's motors.owner values (LINK_SRC_NONE included): a
 * value added to the contract must break this build rather than surface as "?" on the wire. */
_Static_assert(LINK_SRC_SAFE + 2 == MOTORS_OWNER_COUNT, "link_src_t and motors.owner disagree");

/* The actuator task's beat, public because the RT hold is defined against it. */
#define LINK_TICK_MS 20u

/* How long each source's grant holds without being refreshed. The RT hold is one
 * actuator tick PAST the control watchdog's deadline, and necessarily so: the trip
 * must be declared before the grant lapses. The trip revokes the grant itself and
 * hands the actuator to the retreat in the same instant (rt_glue.h), so the retreat's
 * first step follows the stream's last frame with no tick of rest between them — the
 * target it inherits is already zero either way; what the slack buys is that a frame
 * arriving exactly on the deadline refreshes a grant still standing rather than one
 * that lapsed a tick earlier and dipped the duty with no trip at all (test_link.c).
 * rt_link checks silence on a beat of its own, so one tick of slack covers the
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

/* Pure: how far a channel may rise this tick, given who holds the actuator. Every driving
 * source ramps by ramp.rise_ms — the pilot's stream, the retreat, the console. The wizard's
 * identification pulse does not: it is one wheel at ~40 % for LINK_HOLD_CALIB_MS, which is
 * shorter than the slowest ramp — at rise_ms 2000 the channel climbed 40 a tick, reached
 * 1200 of its 1600 in the 30 ticks of the hold and fell to zero, so the "fixed speed" the
 * wizard promises was neither fixed nor reached — and at that scale the ramp buys no safety.
 * The pulse is planned unramped; the pair-mate ordering below still guards the bridge. */
static inline uint16_t link_max_up(link_src_t owner, uint16_t ramp_ms, uint16_t tick_ms) {
    if (owner == LINK_SRC_CALIB) return 4095;
    return ramp_max_up_per_tick(ramp_ms, tick_ms);
}

/* ---- The writer's tick, over an effects table ----------------------------------------
 * What link_task does once it has copied the target out from under the lock: bring the
 * boards up if they are not, otherwise plan, order and write the eight channels, keep the
 * shadows honest and pace the bus reset by the clock. link.c supplies the real table; the
 * host test supplies a recorder — the seam rt_glue.h gives the session lifecycle, given
 * here to the half of link.c that the review found untestable (AJM-127, AJM-91). */

typedef struct {
    void *ctx;                                              /* the recorder in tests; NULL in firmware */
    bool     (*ready)(void *ctx);                           /* pca9685_ready() */
    void     (*zero_all)(void *ctx);                        /* pca9685_zero_all() */
    bool     (*init)(void *ctx);                            /* pca9685_init(BOARD_PWM_HZ) == ESP_OK */
    bool     (*set_pwm)(void *ctx, uint8_t ch, uint16_t d); /* pca9685_set_pwm(ch, d) == ESP_OK */
    void     (*bus_recover)(void *ctx);                     /* pca9685_bus_recover() */
    uint32_t (*now_ms)(void *ctx);                          /* the clock, read where it matters */
} link_tick_fx_t;

/* The bring-up retry and the bus reset share one period, as the spec has it: a second. */
#define LINK_BUS_RETRY_MS 1000u

typedef struct {
    uint16_t cur[8];       /* the shadow: the last duty known to be on the chip, or UNKNOWN */
    uint32_t retry_at;     /* boards down: when the next bring-up attempt may run */
    bool     retry_armed;  /* retry_at holds a deadline; false at boot, so the first tick tries */
    bool     failing;      /* boards up: a write has failed and none has landed since */
    uint32_t recover_at;   /* boards up: when the failing run next earns a bus reset */
} link_tick_t;

static inline void link_tick_init(link_tick_t *t) {
    for (int ch = 0; ch < 8; ch++) t->cur[ch] = LINK_SHADOW_UNKNOWN;
    t->retry_at    = 0;
    t->retry_armed = false;
    t->failing     = false;
    t->recover_at  = 0;
}

typedef enum {
    LINK_TICK_WAITING,      /* boards down, next attempt not due: nothing touched */
    LINK_TICK_STILL_DOWN,   /* an attempt ran — bus reset, zero, init — and failed */
    LINK_TICK_CAME_UP,      /* this attempt brought the boards up: shadows unknown, nothing written yet */
    LINK_TICK_IDLE,         /* every channel on target: nothing written, the bus word is left alone */
    LINK_TICK_WROTE,        /* every write landed: the bus is ok */
    LINK_TICK_FAILED,       /* a write failed: the bus is down */
    LINK_TICK_RESET,        /* writes failing for about a second: the bus was clocked free; still down */
} link_tick_result_t;

static inline link_tick_result_t link_tick_io(link_tick_t *t, const uint16_t tgt[8],
                                              uint16_t max_up, const link_tick_fx_t *fx) {
    /* Nothing is written while the boards are not up, and bringing them up is retried from
       here. pca9685_ready() used to be set once per boot: a single NACK inside the init's ten
       register writes — a marginal bus at power-on — pinned bus_ok false for the whole
       session, while every later write ACKed and the wheels turned anyway on a warm reset.
       Two lies at once. Gating the writes makes the protocol's "bus_ok false means the car
       will not drive" true by construction, and retrying the init makes a transient fault
       cost a second instead of a power cycle. */
    if (!fx->ready(fx->ctx)) {
        uint32_t now = fx->now_ms(fx->ctx);
        if (t->retry_armed && (int32_t)(now - t->retry_at) < 0) return LINK_TICK_WAITING;
        t->retry_armed = true;
        t->retry_at    = now + LINK_BUS_RETRY_MS;
        /* Every attempt here follows a failed one — the boot's, or the last retry's — so the
           bus is clocked free first, the same lever the write path below pulls after a second
           of failures. It used to be pulled only there, which is the one place a car with its
           boards down never reaches: a reset taken mid-transaction leaves a slave holding SDA,
           the boot init times out, and every retry timed out after it — thirty-two zero writes
           and the init's first, once a second, until the battery came off, with the boards
           holding the duty the car crashed at. Unpowered boards get the same reset and do not
           mind: it is a millisecond of clocking on a bus nobody is listening to.

           Zeroed second, always, and before the init. The init ends in RESTART, which resumes
           every channel at its register contents — so the registers are made zero before it.
           zero_all is safe in every state (the LED registers are writable asleep) and is the
           direction of safety anyway. */
        fx->bus_recover(fx->ctx);
        fx->zero_all(fx->ctx);
        if (!fx->init(fx->ctx)) return LINK_TICK_STILL_DOWN;
        /* Unknown, not zero, exactly as link_init leaves them. The zeroing above was not
           checked, and a channel whose zero did not land came back through RESTART at its old
           duty; a shadow of 0 under a target of 0 is a channel the planner never touches, so
           the wheel drove itself under owner idle until some command wanted it nonzero. Unknown
           shadows make the next tick write eight zeros — retried until they land — and the bus
           word turns ok on that evidence, with no command from anyone: boards powered up after
           boot read ok within a tick of coming up, not at the first push of the stick. */
        for (int ch = 0; ch < 8; ch++) t->cur[ch] = LINK_SHADOW_UNKNOWN;
        return LINK_TICK_CAME_UP;
    }

    uint16_t next[8];
    uint8_t  order[8];
    uint8_t  writes = link_plan_writes(t->cur, tgt, max_up, next, order);
    bool wrote = false, failed = false;
    for (uint8_t k = 0; k < writes; k++) {
        uint8_t ch = order[k];
        /* The mate's fall is ordered before this rise; if that write failed the mate still
           shows its old duty here, and the rise waits with it rather than driving both inputs
           of one bridge. */
        if (!link_rise_safe(t->cur[ch ^ 1], next[ch])) continue;
        wrote = true;
        if (fx->set_pwm(fx->ctx, ch, next[ch])) {
            t->cur[ch] = next[ch];      /* shadow follows the chip, not our intent */
        } else {
            /* SHADOW_UNKNOWN, not the old value and not the new one. A failed write is not
               the same as a write that did not happen: pca9685_set_pwm pushes five bytes into
               four auto-incrementing registers and the PCA9685 has no per-channel double
               buffer, so a transfer that aborts partway can leave the channel driving a duty
               neither side asked for — and both i2c attempts can report failure for a transfer
               the peripheral latched. Keeping the OLD value was the bug: the shadow then says 0
               while the chip drives, this channel stops being planned at all (cur == tgt), and
               the pair-mate's next rise passes link_rise_safe(0, x) and drives the other input
               of the same BTS7960. That is the shoot-through motors.h and this file call
               structurally impossible, reached through the one path where the shadow stops
               describing the chip.

               The unknown value is what this file already designed for: it is nonzero, so
               link_rise_safe counts this channel as DRIVING and holds the mate down, and it
               differs from every target, so the next tick still replans and retries. */
            t->cur[ch] = LINK_SHADOW_UNKNOWN;
            failed = true;
        }
    }
    /* Only a tick that actually wrote may call the bus healthy. A tick where every channel
       already sat at its target attempts nothing, and clearing the flag on that evidence
       would report a dead bus as fine the moment the car stood still.

       And only if the boards were actually brought up. A board that a half-finished
       pca9685_init left in SLEEP keeps ACKing every write above — SLEEP gates the PWM
       oscillator, not the I2C interface — so `wrote && !failed` was true on a car whose
       wheels could not turn, and bus_ok latched true. That removed the one signal the app
       has, and contradicted the contract CLAUDE.md states: a motor bus that did not come up
       boots with bus_ok false and the motors inert. pca9685_ready() is the question worth
       asking — and it is asked at the top, where a "no" also skips the writes and retries
       the init, so this point is only ever reached on boards that are up. */
    if (!failed) {
        t->failing = false;
        return wrote ? LINK_TICK_WROTE : LINK_TICK_IDLE;
    }
    /* A wedged bus fails eight channels fifty times a second — but each failing write
       BLOCKS for up to two 50 ms I2C timeouts, so a "tick" under the exact fault
       pca9685_bus_recover exists for (SDA held low) runs 100-800 ms, and pacing by tick
       count turned "speak and recover once a second" into once per 10-40 s while the
       motors held their last duty. Pace by the clock, read after the writes. */
    uint32_t now = fx->now_ms(fx->ctx);
    if (!t->failing) {
        t->failing    = true;
        t->recover_at = now + LINK_BUS_RETRY_MS;      /* first attempt after ~1 s of failure */
        return LINK_TICK_FAILED;
    }
    if ((int32_t)(now - t->recover_at) < 0) return LINK_TICK_FAILED;
    fx->bus_recover(fx->ctx);
    t->recover_at = now + LINK_BUS_RETRY_MS;
    return LINK_TICK_RESET;
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
