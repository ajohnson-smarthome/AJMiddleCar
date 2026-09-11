/* The session lifecycle's SIDE EFFECTS, in order. test_rt_session pins what the flag
 * machine decides; this pins what the task then does to the world — the orderings the
 * cutover plan specifies, which had no host test while test_state.py pinned the same
 * rules on the mock. The recorder is the world: every effect appends its name. */
#define RT_LINK_HOST_TEST
#define RAMP_HOST_TEST
#define LINK_HOST_TEST
#include "../main/rt_glue.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    char       log[8][16];
    int        n;
    bool       stop_ok, release_ok;
    link_src_t owner;
} rec_t;

static void note(rec_t *r, const char *what) {
    snprintf(r->log[r->n], sizeof(r->log[0]), "%s", what);
    if (r->n < 7) r->n++;
}
static bool fx_stop(void *c)      { note(c, "stop");      return ((rec_t *)c)->stop_ok; }
static bool fx_rel_safe(void *c)  { note(c, "rel_safe");  return ((rec_t *)c)->release_ok; }
static bool fx_rel_rt(void *c)    { note(c, "rel_rt");    return true; }
static void fx_forget(void *c)    { note(c, "forget"); }
static void fx_lost(void *c)      { note(c, "lost"); }
static link_src_t fx_owner(void *c) { return ((rec_t *)c)->owner; }

static rec_t R;
static const rt_effects_t FX = { &R, fx_stop, fx_rel_safe, fx_rel_rt,
                                 fx_forget, fx_lost, fx_owner };

static void reset(link_src_t owner) {
    memset(&R, 0, sizeof(R));
    R.stop_ok = R.release_ok = true;
    R.owner = owner;
}
static void expect(int i, const char *what) {
    if (i >= R.n || strcmp(R.log[i], what) != 0) {
        printf("FAIL effect[%d] = '%s', want '%s'\n", i, i < R.n ? R.log[i] : "(none)", what);
        assert(0);
    }
}

int main(void) {
    rt_session_t   s;
    rt_dead_sids_t dead;

    /* --- adopt: evicted sid recorded, SAFE released, path forgotten, then adopt --- */
    memset(&s, 0, sizeof(s)); memset(&dead, 0, sizeof(dead));
    reset(LINK_SRC_NONE);
    rt_session_adopt(&s, "11111111", 100);
    rt_glue_adopt(&s, &dead, "22222222", 200, &FX);
    expect(0, "rel_safe"); expect(1, "forget"); assert(R.n == 2);
    assert(rt_dead_known(&dead, "11111111"));      /* the evicted session is dead */
    assert(!rt_dead_known(&dead, "22222222"));
    assert(s.have_owner && strcmp(s.sid, "22222222") == 0 && s.last_feed_ms == 200);

    /* --- plain goodbye: stop, forget, release, session over, sid dead ------------ */
    reset(LINK_SRC_RT);
    assert(rt_glue_bye(&s, &dead, &FX) == RT_BYE_PLAIN);
    expect(0, "stop"); expect(1, "forget"); expect(2, "rel_safe"); assert(R.n == 3);
    assert(!s.have_owner && !s.armed && !s.have_seq);
    assert(rt_dead_known(&dead, "22222222"));

    /* --- goodbye during an OTA: the sticky hold is NOT grabbed or released -------
       Grabbing SAFE over OTA destroyed "nothing commands the motors during a flash":
       SAFE outranked OTA, the release then left owner NONE, and anything could drive
       for the rest of the write — with esp_restart() landing on a moving car. */
    memset(&s, 0, sizeof(s));
    rt_session_adopt(&s, "33333333", 300);
    reset(LINK_SRC_OTA);
    assert(rt_glue_bye(&s, &dead, &FX) == RT_BYE_UNDER_STICKY);
    expect(0, "forget"); assert(R.n == 1);         /* no stop, no release */
    assert(!s.have_owner);
    assert(rt_dead_known(&dead, "33333333"));

    /* --- goodbye during a wizard pulse: same rule, CALIB self-terminates ---------- */
    memset(&s, 0, sizeof(s));
    rt_session_adopt(&s, "44444444", 400);
    reset(LINK_SRC_CALIB);
    assert(rt_glue_bye(&s, &dead, &FX) == RT_BYE_UNDER_STICKY);
    expect(0, "forget"); assert(R.n == 1);

    /* --- goodbye whose stop is refused is reported, not swallowed ---------------- */
    memset(&s, 0, sizeof(s));
    rt_session_adopt(&s, "55555555", 500);
    reset(LINK_SRC_RT);
    R.stop_ok = false;
    assert(rt_glue_bye(&s, &dead, &FX) == RT_BYE_STOP_REFUSED);
    expect(0, "stop"); expect(1, "forget"); expect(2, "rel_safe");

    /* --- silence: revoke the dead grant, then recovery, then disarm-only trip ---- */
    memset(&s, 0, sizeof(s));
    rt_session_adopt(&s, "66666666", 1000);
    rt_session_command(&s, 41, 1000);
    reset(LINK_SRC_RT);
    assert(!rt_glue_silence(&s, 1000 + RT_WATCHDOG_MS, &FX));      /* in time */
    assert(R.n == 0);
    assert(rt_glue_silence(&s, 1000 + RT_WATCHDOG_MS + 1, &FX));
    expect(0, "rel_rt"); expect(1, "lost"); assert(R.n == 2);
    assert(s.have_owner && !s.armed);
    assert(s.have_seq && s.last_seq == 41);        /* rule 1: the gate survived */

    /* --- mortality: the tripped session ends, its path and sid go with it -------- */
    reset(LINK_SRC_NONE);
    assert(!rt_glue_idle(&s, &dead, 1000 + RT_SESSION_IDLE_MS, &FX));
    assert(R.n == 0);
    assert(rt_glue_idle(&s, &dead, 1001 + RT_SESSION_IDLE_MS, &FX));
    expect(0, "forget"); assert(R.n == 1);
    assert(!s.have_owner && !s.have_seq);
    assert(rt_dead_known(&dead, "66666666"));

    printf("test_rt_glue: all passed\n");
    return 0;
}
