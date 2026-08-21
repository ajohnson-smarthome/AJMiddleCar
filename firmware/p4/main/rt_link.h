#ifndef RT_LINK_H
#define RT_LINK_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "contract.h"        /* RT_PROTO, RT_WATCHDOG_MS */
#include "control_proto.h"   /* control_frame_t, control_seq_newer */
#include "watchdog.h"        /* watchdog_stale */

// The real-time channel: one UDP socket, one task, one owner.
//
// The task's receive timeout is its tick, so the three things that used to live in
// three places happen in one loop — apply the owner's command, notice that the owner
// has gone quiet, and push telemetry back. That is why the control watchdog no longer
// has a file of its own: the task that notices silence is the task that owns the
// channel, rather than a priority-1 timer callback holding the car's safety.

/* ---------------------------------------------------------------------------
 * The session, as flags and arithmetic — everything the channel decides that
 * needs neither a socket nor the actuator. It lives in the header, like
 * watchdog_stale and link_arb_grant, so the lifecycle can be host-tested.
 *
 * It is in the header for a reason: "who owns the actuator after a hello, and
 * after a goodbye" was answered three different ways by three implementations,
 * and two of them grew tests pinning the wrong answer. The rules below are the
 * plan's ("Session lifecycle"), and test_rt_session.c is where they are pinned.
 * ------------------------------------------------------------------------- */

typedef struct {
    bool     have_owner;              /* a session has been adopted */
    char     sid[CONTROL_SID_MAX];    /* its id, as the app spelled it */
    bool     have_seq;                /* last_seq is meaningful */
    uint32_t last_seq;
    bool     armed;                   /* the control watchdog is measuring */
    uint32_t last_feed_ms;            /* when the last accepted command arrived */
} rt_session_t;

/* What one parsed datagram means. The caller does the parts that touch the world:
 * sending the reply, releasing SAFE, clearing the breadcrumbs, driving the motors. */
typedef enum {
    RT_DROP = 0,   /* not our driver, unorderable, or a replay — no state changes */
    RT_REPLY,      /* answer the hello, but do not adopt: a repeat, or a proto we
                      cannot speak */
    RT_ADOPT,      /* this hello becomes the session — and is answered too */
    RT_BYE,
    RT_COMMAND,
} rt_action_t;

/* Pure: classify `f`, which arrived from a peer that either is (`from_owner`) or is not
 * the session's current address. */
static inline rt_action_t rt_session_classify(const rt_session_t *s, bool from_owner,
                                              const control_frame_t *f) {
    if (f->has_hello) {
        /* A hello from a protocol we do not speak is answered by name and not adopted:
           a session neither side can parse is worse than no session, and the reply is
           how the mismatch becomes visible at all. */
        if (!f->has_proto || f->proto != RT_PROTO) return RT_REPLY;
        /* A repeat of the live session's own hello is answered but changes nothing.
           The app repeats the handshake until it is answered, so a retransmission must
           not reset the sequence gate or disarm the watchdog of a session that is
           already driving. */
        if (s->have_owner && from_owner && strcmp(s->sid, f->sid) == 0) return RT_REPLY;
        return RT_ADOPT;   /* a different sid, or a different address: last hello wins */
    }
    if (!s->have_owner || !from_owner) return RT_DROP;   /* not our driver */
    /* Every app->car datagram except a hello carries seq — a goodbye included. One
       without it bypasses replay protection, so it is not acted on. The parser refuses
       these too; the rule is restated here because the ordering test below is
       meaningless without it, and this module does not get to assume its input was
       filtered. */
    if (!f->has_seq) return RT_DROP;
    /* Replay protection is what leaving TCP buys: a reordered or duplicated command
       costs one dropped datagram instead of blocking the queue behind a retransmission. */
    if (s->have_seq && !control_seq_newer(f->seq, s->last_seq)) return RT_DROP;
    if (f->bye)     return RT_BYE;
    if (f->has_ty)  return RT_COMMAND;
    return RT_DROP;
}

/* Adopt `sid` as the session. The sequence gate starts over, and the control watchdog
 * stays DISARMED: it measures the command stream, and the first command is still in
 * flight. Arming here meant a handshake that took longer than RT_WATCHDOG_MS — two lost
 * replies at the app's 5 Hz repeat is enough — declared a loss the session never had.
 * The caller pairs this with releasing SAFE and clearing the breadcrumb history. */
static inline void rt_session_adopt(rt_session_t *s, const char *sid) {
    s->have_owner = true;
    s->have_seq   = false;
    s->armed      = false;
    snprintf(s->sid, sizeof(s->sid), "%s", sid);
}

/* An accepted command: the only thing that arms the watchdog, and the only thing that
 * refreshes it. Actuator health is a separate question, answered by bus_ok. */
static inline void rt_session_command(rt_session_t *s, uint32_t seq, uint32_t now_ms) {
    s->last_seq     = seq;
    s->have_seq     = true;
    s->last_feed_ms = now_ms;
    s->armed        = true;
}

/* A goodbye: silence that was announced is not silence that means the driver is out of
 * range, and ownership is not resumable — the next session needs a fresh hello. */
static inline void rt_session_bye(rt_session_t *s) {
    s->armed      = false;
    s->have_owner = false;
    s->have_seq   = false;
}

/* The watchdog tripped. Ownership of the *channel* is deliberately kept: a stream that
 * resumes after a dropout is the same session. The sequence gate is not — silence past
 * the deadline already proves the stream is dead, so there is nothing left to
 * replay-protect, and a gate left desynchronised (one spoofed frame far in the future,
 * or a counter bug) would drop every genuine frame for the rest of the session while
 * telemetry kept flowing and the app showed a healthy link. */
static inline void rt_session_trip(rt_session_t *s) {
    s->armed    = false;
    s->have_seq = false;
}

/* Pure: has an armed session gone quiet past the contract's deadline? */
static inline bool rt_session_lost(const rt_session_t *s, uint32_t now_ms) {
    return s->armed && watchdog_stale(s->last_feed_ms, now_ms, RT_WATCHDOG_MS);
}

#ifndef RT_LINK_HOST_TEST
#include "esp_err.h"

// Call after wifi_ap_start() and recovery_init(); it needs neither the HTTP server nor
// the console.
esp_err_t rt_link_start(void);

// Control frames accepted since boot — telemetry's rx_fps is the derivative of this.
// Written only by the rt_link task, read by whoever gathers telemetry; an aligned u32
// load is atomic, and a reader that is one frame behind reports a rate, not a fact.
uint32_t rt_link_frames(void);

// How many times the control watchdog has declared the link lost since boot. An
// increment is the difference between a driver who said goodbye and one who walked out
// of range, so the app plots it rather than merely logging it.
uint32_t rt_link_wdt_trips(void);
#endif /* RT_LINK_HOST_TEST */

#endif // RT_LINK_H
