/* The session lifecycle: which datagram is acted on, and what a hello, a command, a
 * goodbye and a watchdog trip do to the channel's state.
 *
 * This is the file that should have existed before the cutover. "Who owns the actuator
 * after a hello, and after a goodbye" was answered three different ways by three
 * implementations of one wire, and two of them then grew tests pinning their own answer
 * as correct. The answer is the plan's ("Session lifecycle — who owns the actuator, and
 * when"), and every assertion below is one sentence of it.
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

/* Two sessions, and a datagram from each. proto is 1 because the wire says 1; if the
   contract ever bumps it, this file is one of the places that has to be read. */
#define HELLO_A   "{\"proto\":1,\"hello\":\"7f3a91c2\"}"
#define HELLO_A2  "{\"proto\":1,\"hello\":\"7f3a91c2\"}"   /* the app's repeat, verbatim */
#define HELLO_B   "{\"proto\":1,\"hello\":\"0b17ac55\"}"
#define SID_A     "7f3a91c2"

static control_frame_t frame(const char *msg) {
    control_frame_t f;
    if (control_parse_frame(msg, strlen(msg), RT_MAX_COMMAND, &f) != 0) {
        printf("FAIL the parser refused '%s'\n", msg);
        assert(0);
    }
    return f;
}

/* What the car does with `msg`, arriving from the session's address or from a stranger. */
static rt_action_t act(const rt_session_t *s, bool from_owner, const char *msg) {
    control_frame_t f = frame(msg);
    return rt_session_classify(s, from_owner, &f);
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
    /* A car nobody has connected to never "loses" a link it never had. */
    assert(!rt_session_lost(&s, 0));
    assert(!rt_session_lost(&s, 1000000));
    assert(act(&s, false, "{\"seq\":1,\"t\":1,\"y\":0}") == RT_DROP);

    /* --- adoption ------------------------------------------------------------ */
    assert(act(&s, false, HELLO_A) == RT_ADOPT);
    rt_session_adopt(&s, SID_A);
    assert(s.have_owner && strcmp(s.sid, SID_A) == 0);
    assert(!s.have_seq);                       /* the new session counts from anywhere */

    /* THE regression. The watchdog measures the command stream, and the first command
       is still in flight: a handshake that takes longer than the deadline (two lost
       replies at the app's 5 Hz repeat is enough) must not declare a loss and send the
       car retracing the PREVIOUS session's path. */
    assert(!s.armed);
    assert(!rt_session_lost(&s, RT_WATCHDOG_MS + 1));
    assert(!rt_session_lost(&s, RT_WATCHDOG_MS * 100));

    /* The app repeats the handshake until it is answered. A repeat from the same peer
       with the same sid is answered and nothing else — re-adopting would reset the
       sequence gate of a session that is already driving. */
    assert(act(&s, true, HELLO_A2) == RT_REPLY);
    /* A different sid, or the same sid from a different address, is a different
       session: last hello wins, which is what "strictly one client" means here. */
    assert(act(&s, true, HELLO_B) == RT_ADOPT);
    assert(act(&s, false, HELLO_A2) == RT_ADOPT);

    /* A protocol we cannot speak is answered by name and never adopted — the reply is
       how a flashed-but-not-updated pair finds out, instead of searching forever. */
    assert(act(&s, false, "{\"proto\":2,\"hello\":\"deadbeef\"}") == RT_REPLY);
    assert(act(&s, true,  "{\"proto\":2,\"hello\":\"deadbeef\"}") == RT_REPLY);
    assert(act(&s, false, "{\"hello\":\"deadbeef\"}") == RT_REPLY);   /* no proto at all */

    /* --- commands ------------------------------------------------------------ */
    assert(act(&s, true, "{\"seq\":10,\"t\":0.5,\"y\":0}") == RT_COMMAND);
    assert(act(&s, false, "{\"seq\":10,\"t\":0.5,\"y\":0}") == RT_DROP);   /* not our driver */
    rt_session_command(&s, 10, 1000);
    assert(s.armed && s.have_seq && s.last_seq == 10 && s.last_feed_ms == 1000);

    /* The deadline is the contract's, and the comparison is "more than": a frame that
       arrives exactly on it is still in time. */
    assert(!rt_session_lost(&s, 1000 + RT_WATCHDOG_MS));
    assert(rt_session_lost(&s, 1000 + RT_WATCHDOG_MS + 1));

    /* Replay protection: a duplicate or a reordered datagram costs one dropped frame. */
    assert(act(&s, true, "{\"seq\":10,\"t\":1,\"y\":0}") == RT_DROP);
    assert(act(&s, true, "{\"seq\":9,\"t\":1,\"y\":0}") == RT_DROP);
    assert(act(&s, true, "{\"seq\":11,\"t\":1,\"y\":0}") == RT_COMMAND);

    /* ...and it wraps, because a uint32 at 10 Hz does eventually. */
    rt_session_command(&s, 0xFFFFFFFFu, 2000);
    assert(act(&s, true, "{\"seq\":0,\"t\":0,\"y\":0}") == RT_COMMAND);
    assert(act(&s, true, "{\"seq\":4294967295,\"t\":0,\"y\":0}") == RT_DROP);

    /* --- the datagrams the car will not act on -------------------------------- */
    /* Every app->car datagram except a hello carries seq. The parser refuses these, and
       the session's own gate refuses them again — the two halves disagreeing about
       exactly this is what put a goodbye on the wire that the car parsed and dropped. */
    refused("{\"t\":0,\"y\":0}");
    refused("{\"bye\":true}");
    control_frame_t bare = { .bye = true };     /* has_seq false, by hand */
    assert(rt_session_classify(&s, true, &bare) == RT_DROP);

    /* --- goodbye -------------------------------------------------------------- */
    rt_session_command(&s, 100, 3000);
    assert(act(&s, true, "{\"seq\":101,\"t\":0,\"y\":0,\"bye\":1}") == RT_BYE);
    rt_session_bye(&s);
    assert(!s.armed && !s.have_owner && !s.have_seq);
    /* Announced silence is not a loss: no trip, no retreat, however long it lasts. */
    assert(!rt_session_lost(&s, 3000 + RT_WATCHDOG_MS + 1));
    assert(!rt_session_lost(&s, 3000 + RT_WATCHDOG_MS * 1000));
    /* Ownership is not resumable — the next session arrives with a fresh hello. */
    assert(act(&s, true, "{\"seq\":102,\"t\":1,\"y\":0}") == RT_DROP);
    assert(act(&s, true, HELLO_A) == RT_ADOPT);

    /* --- the watchdog trip ---------------------------------------------------- */
    rt_session_adopt(&s, SID_A);
    rt_session_command(&s, 500, 10000);
    assert(rt_session_lost(&s, 10000 + RT_WATCHDOG_MS + 1));
    rt_session_trip(&s);
    /* The channel's owner is kept: a stream that resumes after a dropout is the same
       session, and dropping it would ignore the driver until the app noticed. */
    assert(s.have_owner && strcmp(s.sid, SID_A) == 0);
    /* One loss, one trip: disarmed until traffic returns. */
    assert(!s.armed && !rt_session_lost(&s, 10000 + RT_WATCHDOG_MS * 100));
    /* And the sequence gate goes with it. Silence past the deadline already proved the
       stream was dead; a gate left behind would drop every genuine frame of whatever
       resumes, while telemetry kept flowing and the app showed a healthy link. */
    assert(!s.have_seq);
    assert(act(&s, true, "{\"seq\":3,\"t\":0.2,\"y\":0}") == RT_COMMAND);

    /* --- adoption clears the gate too ----------------------------------------- */
    rt_session_command(&s, 900, 20000);
    rt_session_adopt(&s, "0b17ac55");
    assert(act(&s, true, "{\"seq\":1,\"t\":0,\"y\":0}") == RT_COMMAND);

    printf("test_rt_session: all passed\n");
    return 0;
}
