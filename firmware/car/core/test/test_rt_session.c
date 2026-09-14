/* The session lifecycle: which datagram is acted on, and what a hello, a command, a
 * goodbye and a watchdog trip do to the channel's state.
 *
 * The datagrams are written out as wire bytes on purpose: a test that built frames from
 * the same symbols the parser uses would agree with a typo. The key names are generated
 * (RT_KEY_*) and the parser is the one under test, so these strings are the golden copy.
 */
#define RT_LINK_HOST_TEST
#include "../main/rt_link.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* proto is 2 because the wire says 2; if the contract ever bumps it, this file is one of
   the places that has to be read. */
#define HELLO_A   "{\"proto\":2,\"type\":\"hello\",\"session\":\"7f3a91c2\"}"
#define HELLO_A2  HELLO_A                        /* the app's repeat, verbatim */
#define HELLO_B   "{\"proto\":2,\"type\":\"hello\",\"session\":\"0b17ac55\"}"
#define SID_A     "7f3a91c2"
#define DRIVE(seq, t, y) \
    "{\"proto\":2,\"type\":\"drive\",\"seq\":" #seq ",\"throttle\":" #t ",\"turn\":" #y "}"
#define BYE(seq) "{\"proto\":2,\"type\":\"bye\",\"seq\":" #seq "}"

static control_frame_t frame(const char *msg) {
    control_frame_t f;
    if (control_parse_frame(msg, strlen(msg), RT_MAX_COMMAND, &f) != 0) {
        printf("FAIL the parser refused '%s'\n", msg);
        assert(0);
    }
    return f;
}

/* What the car does with `msg`, arriving from the session's address or from a stranger.
   `dead` is the dead-sid ring in play, or NULL when a test has no rule 3 to apply. */
static rt_action_t act(const rt_session_t *s, const rt_dead_sids_t *dead,
                       bool from_owner, const char *msg) {
    control_frame_t f = frame(msg);
    return rt_session_classify(s, dead, from_owner, &f);
}

static void refused(const char *msg) {
    control_frame_t f;
    if (control_parse_frame(msg, strlen(msg), RT_MAX_COMMAND, &f) == 0) {
        printf("FAIL the parser accepted '%s'\n", msg);
        assert(0);
    }
}

int main(void) {
    rt_session_t s = {0};

    /* --- before anyone has said hello ---------------------------------------- */
    assert(!rt_session_lost(&s, 0));
    assert(!rt_session_lost(&s, 1000000));
    assert(act(&s, NULL, false, DRIVE(1, 1, 0)) == RT_DROP);

    /* --- adoption ------------------------------------------------------------ */
    assert(act(&s, NULL, false, HELLO_A) == RT_ADOPT);
    rt_session_adopt(&s, SID_A, 500);
    assert(s.last_feed_ms == 500);
    assert(s.have_owner && strcmp(s.sid, SID_A) == 0);
    assert(!s.have_seq);
    assert(!s.armed);
    assert(!rt_session_lost(&s, RT_WATCHDOG_MS + 1));
    assert(!rt_session_lost(&s, RT_WATCHDOG_MS * 100));

    assert(act(&s, NULL, true, HELLO_A2) == RT_REPLY);
    assert(act(&s, NULL, true, HELLO_B) == RT_ADOPT);
    assert(act(&s, NULL, false, HELLO_A2) == RT_ADOPT);

    /* A protocol we cannot speak is answered by name and never adopted — the reply is
       how a flashed-but-not-updated pair finds out, instead of searching forever. */
    assert(act(&s, NULL, false, "{\"proto\":3,\"type\":\"hello\",\"session\":\"deadbeef\"}") == RT_REPLY);
    assert(act(&s, NULL, true,  "{\"proto\":1,\"type\":\"hello\",\"session\":\"deadbeef\"}") == RT_REPLY);
    assert(act(&s, NULL, false, "{\"type\":\"hello\",\"session\":\"deadbeef\"}") == RT_REPLY);

    /* --- commands ------------------------------------------------------------ */
    assert(act(&s, NULL, true, DRIVE(10, 0.5, 0)) == RT_COMMAND);
    assert(act(&s, NULL, false, DRIVE(10, 0.5, 0)) == RT_DROP);        /* not our driver */
    /* proto on every datagram, not only the hello: a drive in a dialect we do not speak
       is dropped, not driven on. Absent counts as foreign — v2 requires it. */
    assert(act(&s, NULL, true, "{\"proto\":1,\"type\":\"drive\",\"seq\":10,\"throttle\":0.5,\"turn\":0}") == RT_DROP);
    assert(act(&s, NULL, true, "{\"type\":\"drive\",\"seq\":10,\"throttle\":0.5,\"turn\":0}") == RT_DROP);
    rt_session_command(&s, 10, 1000);
    assert(s.armed && s.have_seq && s.last_seq == 10 && s.last_feed_ms == 1000);
    assert(!rt_session_lost(&s, 1000 + RT_WATCHDOG_MS));
    assert(rt_session_lost(&s, 1000 + RT_WATCHDOG_MS + 1));

    assert(act(&s, NULL, true, DRIVE(10, 1, 0)) == RT_DROP);
    assert(act(&s, NULL, true, DRIVE(9, 1, 0)) == RT_DROP);
    assert(act(&s, NULL, true, DRIVE(11, 1, 0)) == RT_COMMAND);

    rt_session_command(&s, 0xFFFFFFFFu, 2000);
    assert(act(&s, NULL, true, DRIVE(0, 0, 0)) == RT_COMMAND);
    assert(act(&s, NULL, true, DRIVE(4294967295, 0, 0)) == RT_DROP);

    /* --- the datagrams the car will not act on -------------------------------- */
    refused("{\"proto\":2,\"type\":\"drive\",\"throttle\":0,\"turn\":0}");
    refused("{\"proto\":2,\"type\":\"bye\"}");
    control_frame_t bare = { .type = CT_BYE, .has_proto = true, .proto = RT_PROTO };  /* has_seq false, by hand */
    assert(rt_session_classify(&s, NULL, true, &bare) == RT_DROP);
    control_frame_t none = { .type = CT_NONE, .has_proto = true, .proto = RT_PROTO, .has_seq = true, .seq = 12 };
    assert(rt_session_classify(&s, NULL, true, &none) == RT_DROP);
    /* view is hello-shaped but not a hello: on 4210 it is just another non-hello without
       seq, and the existing rule drops it. */
    assert(act(&s, NULL, true, "{\"proto\":2,\"type\":\"view\",\"session\":\"7f3a91c2\"}") == RT_DROP);

    /* --- goodbye -------------------------------------------------------------- */
    rt_session_command(&s, 100, 3000);
    assert(act(&s, NULL, true, BYE(101)) == RT_BYE);
    assert(act(&s, NULL, true, "{\"proto\":1,\"type\":\"bye\",\"seq\":102}") == RT_DROP);  /* foreign proto */
    rt_session_bye(&s);
    assert(!s.armed && !s.have_owner && !s.have_seq);
    assert(!rt_session_lost(&s, 3000 + RT_WATCHDOG_MS + 1));
    assert(!rt_session_lost(&s, 3000 + RT_WATCHDOG_MS * 1000));
    assert(act(&s, NULL, true, DRIVE(102, 1, 0)) == RT_DROP);
    assert(act(&s, NULL, true, HELLO_A) == RT_ADOPT);

    /* --- the watchdog trip ---------------------------------------------------- */
    rt_session_adopt(&s, SID_A, 9000);
    rt_session_command(&s, 500, 10000);
    assert(rt_session_lost(&s, 10000 + RT_WATCHDOG_MS + 1));
    rt_session_trip(&s);
    assert(s.have_owner && strcmp(s.sid, SID_A) == 0);
    assert(!s.armed && !rt_session_lost(&s, 10000 + RT_WATCHDOG_MS * 100));
    assert(s.have_seq && s.last_seq == 500);
    assert(act(&s, NULL, true, DRIVE(3, 0.2, 0)) == RT_DROP);
    assert(act(&s, NULL, true, DRIVE(501, 0.2, 0)) == RT_COMMAND);

    /* --- adoption clears the gate too ----------------------------------------- */
    rt_session_command(&s, 900, 20000);
    rt_session_adopt(&s, "0b17ac55", 21000);
    assert(act(&s, NULL, true, DRIVE(1, 0, 0)) == RT_COMMAND);

    /* --- dead sids: a stale hello cannot evict a live driver (rule 3) -------- */
    rt_dead_sids_t dead = {0};
    rt_dead_note(&dead, "deadbee1");
    assert(rt_dead_known(&dead, "deadbee1"));
    assert(!rt_dead_known(&dead, "7f3a91c2"));
    rt_dead_note(&dead, "deadbee2");
    rt_dead_note(&dead, "deadbee3");
    rt_dead_note(&dead, "deadbee4");
    rt_dead_note(&dead, "deadbee5");
    assert(!rt_dead_known(&dead, "deadbee1"));
    assert(rt_dead_known(&dead, "deadbee5"));

    rt_session_t live = {0};
    rt_session_adopt(&live, "0b17ac55", 1000);
    rt_dead_note(&dead, "deadsid1");
    assert(act(&live, &dead, false, "{\"proto\":2,\"type\":\"hello\",\"session\":\"deadsid1\"}") == RT_REPLY);
    assert(act(&live, &dead, false, HELLO_A) == RT_ADOPT);
    rt_session_t empty = {0};
    assert(act(&empty, &dead, false, "{\"proto\":2,\"type\":\"hello\",\"session\":\"deadsid1\"}") == RT_ADOPT);
    rt_dead_note(&dead, "0b17ac55");
    assert(act(&live, &dead, true, "{\"proto\":2,\"type\":\"hello\",\"session\":\"0b17ac55\"}") == RT_REPLY);

    /* --- mortality (rule 4) --------------------------------------------------- */
    rt_session_t m = {0};
    assert(!rt_session_idle(&m, 999999));
    rt_session_adopt(&m, "7f3a91c2", 2000);
    assert(!rt_session_idle(&m, 2000 + RT_SESSION_IDLE_MS));
    assert(rt_session_idle(&m, 2000 + RT_SESSION_IDLE_MS + 1));
    rt_session_command(&m, 7, 5000);
    assert(!rt_session_idle(&m, 5000 + 100));
    assert(!rt_session_idle(&m, 5000 + RT_SESSION_IDLE_MS + 1));
    rt_session_trip(&m);
    assert(!rt_session_idle(&m, 5000 + RT_SESSION_IDLE_MS));
    assert(rt_session_idle(&m, 5000 + RT_SESSION_IDLE_MS + 1));
    rt_session_bye(&m);
    assert(!rt_session_idle(&m, 5000 + 2 * RT_SESSION_IDLE_MS));

    printf("test_rt_session: all passed\n");
    return 0;
}
