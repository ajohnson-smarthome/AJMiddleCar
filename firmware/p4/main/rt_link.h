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

/* The sids of recently ended sessions — ended by a goodbye, by eviction, or by idling
 * out. A hello carrying one of these is answered but not re-adopted while a live
 * session exists: UDP duplicates a hello as happily as a command, and an un-gated
 * replay of a dead session's handshake evicted the live driver for the ~3 s the app
 * takes to notice and re-hello. Four is deep enough for every stale duplicate a real
 * network holds; the app never reuses a sid, so a collision is a replay by definition. */
#define RT_DEAD_SIDS 4
typedef struct {
    char    sid[RT_DEAD_SIDS][CONTROL_SID_MAX];
    uint8_t next;
} rt_dead_sids_t;

static inline void rt_dead_note(rt_dead_sids_t *d, const char *sid) {
    snprintf(d->sid[d->next], CONTROL_SID_MAX, "%s", sid);
    d->next = (uint8_t)((d->next + 1) % RT_DEAD_SIDS);
}

static inline bool rt_dead_known(const rt_dead_sids_t *d, const char *sid) {
    for (int i = 0; i < RT_DEAD_SIDS; i++) {
        if (d->sid[i][0] != '\0' && strcmp(d->sid[i], sid) == 0) return true;
    }
    return false;
}

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
 * the session's current address. `dead` is the ring of recently ended sessions' sids —
 * NULL when the caller has no rule 3 to apply. */
static inline rt_action_t rt_session_classify(const rt_session_t *s,
                                              const rt_dead_sids_t *dead,
                                              bool from_owner,
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
        /* A dead session's replayed hello must not evict a live driver. With no live
           session there is nobody to protect, so the sid may return — refusing it
           would wedge a client whose session idled out mid-handshake. */
        if (s->have_owner && dead != NULL && rt_dead_known(dead, f->sid)) return RT_REPLY;
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
 * The caller pairs this with releasing SAFE and clearing the breadcrumb history.
 * `now_ms` stamps last_feed_ms so mortality (rt_session_idle) has a start point even for
 * a session that never gets a command before it idles out. */
static inline void rt_session_adopt(rt_session_t *s, const char *sid, uint32_t now_ms) {
    s->have_owner   = true;
    s->have_seq     = false;
    s->armed        = false;
    s->last_feed_ms = now_ms;   /* mortality (rt_session_idle) counts from here */
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
 * resumes after a dropout is the same session. The sequence gate is ALSO kept — a
 * resuming stream carries monotonically newer seqs and passes it, while clearing it
 * accepted one network-delayed pre-dropout duplicate as the resumed stream: re-armed
 * watchdog, aborted retreat, stale stick values held for a whole grant. (The old text
 * here argued the opposite from a desync that cannot happen to a well-behaved client;
 * a session whose counter really is broken ends at RT_SESSION_IDLE_MS.) */
static inline void rt_session_trip(rt_session_t *s) {
    s->armed = false;
}

/* Pure: has an armed session gone quiet past the contract's deadline? */
static inline bool rt_session_lost(const rt_session_t *s, uint32_t now_ms) {
    return s->armed && watchdog_stale(s->last_feed_ms, now_ms, RT_WATCHDOG_MS);
}

/* Pure: has an unarmed session gone without an accepted command for so long that it is
 * dead rather than merely quiet? Armed sessions belong to the watchdog; this begins
 * where the trip ends, and it is what stops the car pushing telemetry to a vanished
 * address forever — and what bounds the window in which a stale datagram could ever
 * find an owner to impersonate. */
static inline bool rt_session_idle(const rt_session_t *s, uint32_t now_ms) {
    return s->have_owner && !s->armed &&
           watchdog_stale(s->last_feed_ms, now_ms, RT_SESSION_IDLE_MS);
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
