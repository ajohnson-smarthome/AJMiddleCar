#ifndef RT_GLUE_H
#define RT_GLUE_H

#include "rt_link.h"
#include "link.h"

/* The session lifecycle's side effects, in the order the cutover plan specifies, over
 * an effects table instead of the live modules. rt_link.c supplies the real table; the
 * host test supplies a recorder. This is the seam the audit found missing: adopt,
 * goodbye and the trip each order calls into link/recovery/car, the mock pins those
 * orderings in test_state.py, and the firmware's copies had no test at all — so a
 * reorder (release SAFE before forgetting the path, say) shipped silently. */

typedef struct {
    void *ctx;                             /* the recorder in tests; NULL in firmware */
    bool (*stop_safe)(void *ctx);          /* car_stop(LINK_SRC_SAFE) */
    bool (*release_safe)(void *ctx);       /* link_release_must(LINK_SRC_SAFE) */
    bool (*release_rt)(void *ctx);         /* link_release_must(LINK_SRC_RT) */
    void (*forget)(void *ctx);             /* recovery_forget() */
    void (*on_link_lost)(void *ctx);       /* recovery_on_link_lost() */
    link_src_t (*owner)(void *ctx);        /* link_owner() */
} rt_effects_t;

/* Adopt `sid`: the evicted session's sid (if any, and different) is recorded dead, a
 * SAFE left by a previous goodbye is released, the previous driver's path is thrown
 * away, and only then does the session change hands. Returns the release's verdict so
 * the caller can log; the effects table's release already retries and logs itself. */
static inline bool rt_glue_adopt(rt_session_t *s, rt_dead_sids_t *dead,
                                 const char *sid, uint32_t now,
                                 const rt_effects_t *fx) {
    if (s->have_owner && strcmp(s->sid, sid) != 0) rt_dead_note(dead, s->sid);
    bool released = fx->release_safe(fx->ctx);
    fx->forget(fx->ctx);
    rt_session_adopt(s, sid, now);
    return released;
}

typedef enum {
    RT_BYE_PLAIN,          /* stopped, forgotten, released — the common case */
    RT_BYE_UNDER_STICKY,   /* OTA or CALIB holds the actuator: hands off it */
    RT_BYE_STOP_REFUSED,   /* the SAFE stop did not land; the grant lapse still stops */
} rt_bye_result_t;

/* A goodbye. When OTA or the wizard's pulse holds the actuator, the SAFE grab-and-
 * release is skipped entirely: the motors are already stopped (OTA) or deliberately
 * moving (CALIB), and grabbing SAFE over a sticky OTA destroyed "nothing may command
 * the motors during a flash" — SAFE outranked it, the release left owner NONE, and the
 * end-of-flash esp_restart() could land on a car someone had started driving again.
 * Clearing the breadcrumbs is what actually suppresses the retreat, and that happens
 * in every case; so does ending the session and recording its sid dead. */
static inline rt_bye_result_t rt_glue_bye(rt_session_t *s, rt_dead_sids_t *dead,
                                          const rt_effects_t *fx) {
    link_src_t o = fx->owner(fx->ctx);
    rt_bye_result_t r;
    if (o == LINK_SRC_OTA || o == LINK_SRC_CALIB) {
        r = RT_BYE_UNDER_STICKY;
    } else {
        r = fx->stop_safe(fx->ctx) ? RT_BYE_PLAIN : RT_BYE_STOP_REFUSED;
    }
    fx->forget(fx->ctx);
    if (r != RT_BYE_UNDER_STICKY) fx->release_safe(fx->ctx);
    rt_dead_note(dead, s->sid);
    rt_session_bye(s);
    return r;
}

/* The silence check: on a trip, revoke the dead stream's grant explicitly (waiting for
 * it to lapse at this same instant would refuse the retreat's first step), hand off to
 * recovery, and disarm. The sequence gate survives — see rt_session_trip. */
static inline bool rt_glue_silence(rt_session_t *s, uint32_t now, const rt_effects_t *fx) {
    if (!rt_session_lost(s, now)) return false;
    fx->release_rt(fx->ctx);
    fx->on_link_lost(fx->ctx);
    rt_session_trip(s);
    return true;
}

/* Mortality: a session that has not commanded for RT_SESSION_IDLE_MS ends. Its path
 * goes with it (a retreat must never retrace a dead driver's drive), its sid is
 * recorded dead, and the telemetry push stops because have_owner is what gates it.
 * The resuming client says hello again; the app already does after its 3 s stall. */
static inline bool rt_glue_idle(rt_session_t *s, rt_dead_sids_t *dead, uint32_t now,
                                const rt_effects_t *fx) {
    if (!rt_session_idle(s, now)) return false;
    fx->forget(fx->ctx);
    rt_dead_note(dead, s->sid);
    s->have_owner = false;
    s->have_seq   = false;
    return true;
}

#endif /* RT_GLUE_H */
