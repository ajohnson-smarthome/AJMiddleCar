#include "control_proto.h"
#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
/* The cap is the schema's, not this test's — a frame that is legal here and rejected
   on the car (or the reverse) is exactly the class of bug the generator exists to
   remove. It is RT_MAX_COMMAND, the largest datagram the car accepts; RT_MAX_DATAGRAM
   is the wider receive buffer that telemetry needs and is not a licence to command. */
#include "cfg_table.inc"

static int approx(float a, float b) { return fabsf(a - b) < 1e-4f; }

static control_frame_t parse(const char *msg) {
    control_frame_t f;
    int r = control_parse_frame(msg, strlen(msg), RT_MAX_COMMAND, &f);
    if (r != 0) {
        printf("FAIL parse('%s') -> %d, want 0\n", msg, r);
        assert(0);
    }
    return f;
}

static void ok(const char *msg, float et, float ey) {
    control_frame_t f = parse(msg);
    if (!f.has_ty || !approx(f.t, et) || !approx(f.y, ey)) {
        printf("FAIL ok('%s') -> has_ty=%d t=%.4f y=%.4f (want t=%.4f y=%.4f)\n",
               msg, f.has_ty, f.t, f.y, et, ey);
        assert(0);
    }
}

static void bad(const char *msg) {
    control_frame_t f;
    size_t len = msg ? strlen(msg) : 0;
    int r = control_parse_frame(msg, len, RT_MAX_COMMAND, &f);
    if (r != -1) {
        printf("FAIL bad('%s') -> r=%d, want -1\n", msg ? msg : "(null)", r);
        assert(0);
    }
}

static void seq_newer(uint32_t seq, uint32_t last, bool want) {
    if (control_seq_newer(seq, last) != want) {
        printf("FAIL seq_newer(%u,%u) != %d\n", seq, last, want);
        assert(0);
    }
}

int main(void) {
    /* --- commands, as the app sends them ------------------------------------ */
    ok("{\"seq\":1,\"t\":0.5,\"y\":0}", 0.5f, 0.0f);
    ok("{\"t\":0,\"y\":1}", 0.0f, 1.0f);
    ok("{\"t\":-1,\"y\":-0.5}", -1.0f, -0.5f);
    ok("{\"y\":-1.0,\"t\":1.0}", 1.0f, -1.0f);            // key order independent
    ok("{ \"t\" : 0.25 , \"y\" : 0.75 }", 0.25f, 0.75f);  // whitespace tolerated

    control_frame_t f = parse("{\"seq\":1234,\"t\":0.50,\"y\":-0.25}");
    assert(f.has_seq && f.seq == 1234 && !f.bye && !f.has_hello && !f.has_proto);

    /* A command with no seq parses — the transport, not the parser, decides that a
       command it cannot order is one it will not apply. */
    f = parse("{\"t\":0,\"y\":0}");
    assert(!f.has_seq);

    /* --- hello --------------------------------------------------------------- */
    f = parse("{\"proto\":1,\"hello\":\"7f3a91c2\"}");
    assert(f.has_hello && strcmp(f.sid, "7f3a91c2") == 0);
    assert(f.has_proto && f.proto == RT_PROTO && !f.has_ty && !f.has_seq);

    /* A hello from a future protocol still parses: the car has to be able to answer it
       by name rather than ignore it. */
    f = parse("{\"proto\":99,\"hello\":\"abc123\"}");
    assert(f.has_proto && f.proto == 99);

    bad("{\"proto\":1,\"hello\":\"\"}");            // empty sid — no session to name
    bad("{\"proto\":1}");                           // hello missing entirely
    bad("{\"hello\":\"7f3a91c2");                   // unterminated string
    bad("{\"hello\":7}");                           // not a string
    bad("{\"hello\":\"0123456789abcdefgh\"}");      // longer than CONTROL_SID_MAX
    /* The id is echoed into the hello reply, so anything that could reshape that JSON
       is refused rather than escaped. */
    bad("{\"hello\":\"7f3a\\\"91\"}");
    bad("{\"hello\":\"a b\"}");

    /* --- goodbye ------------------------------------------------------------- */
    f = parse("{\"seq\":1235,\"t\":0,\"y\":0,\"bye\":1}");
    assert(f.bye && f.has_seq && f.seq == 1235 && f.has_ty);
    f = parse("{\"seq\":2,\"t\":0,\"y\":0,\"bye\":0}");
    assert(!f.bye);                                  // present but zero is not a goodbye
    f = parse("{\"seq\":3,\"t\":0,\"y\":0,\"bye\":true}");
    assert(f.bye);                                   // JSON's other way of saying yes
    f = parse("{\"seq\":4,\"t\":0,\"y\":0,\"bye\":false}");
    assert(!f.bye);
    bad("{\"seq\":5,\"t\":0,\"y\":0,\"bye\":\"yes\"}");   // but not any way at all

    /* A goodbye is worth acting on with no axes at all: the alternative is dropping it
       at the parser, and then the car waits out the deadline and retreats — the one
       thing a goodbye exists to prevent. */
    f = parse("{\"seq\":6,\"bye\":1}");
    assert(f.bye && f.has_seq && f.seq == 6 && !f.has_ty);
    f = parse("{\"bye\":true}");
    assert(f.bye);
    bad("{\"seq\":7,\"bye\":0}");                      // "not a goodbye" is not a command

    /* --- seq comparison, including the wrap ---------------------------------- */
    seq_newer(2, 1, true);
    seq_newer(1, 1, false);                          // a repeat is not newer
    seq_newer(1, 2, false);                          // reordered, drop it
    seq_newer(0, 0xFFFFFFFFu, true);                 // the wrap is newer, not 4 billion older
    seq_newer(5, 0xFFFFFFF0u, true);
    seq_newer(0xFFFFFFFFu, 0, false);
    seq_newer(0x80000000u, 0, false);                // exactly half a lap: refuse to guess

    /* A command may not use the whole receive buffer: the two caps differ, and the one
       the car enforces is the smaller. */
    {
        char over[RT_MAX_DATAGRAM];
        const char *h = "{\"seq\":1,\"t\":0,\"y\":0,\"pad\":\"";
        int m = (int)strlen(h);
        memcpy(over, h, (size_t)m);
        while (m < RT_MAX_DATAGRAM - 2) over[m++] = 'x';
        over[m++] = '"';
        over[m++] = '}';
        control_frame_t o2;
        assert(control_parse_frame(over, (size_t)m, RT_MAX_COMMAND, &o2) == -1);
        assert(control_parse_frame(over, (size_t)m, RT_MAX_DATAGRAM, &o2) == 0);
    }

    /* --- the command cap ------------------------------------------------------ */
    /* A well-formed frame padded to exactly the cap, and the same frame one byte over.
       The rule is "larger than", and an off-by-one here silently costs the wire a byte. */
    char big[RT_MAX_COMMAND * 2];
    const char *head = "{\"seq\":1,\"t\":0.5,\"y\":0.5,\"pad\":\"";
    int n = (int)strlen(head);
    memcpy(big, head, (size_t)n);
    while (n < RT_MAX_COMMAND - 2) big[n++] = 'x';
    big[n++] = '"';
    big[n++] = '}';
    assert(n == RT_MAX_COMMAND);
    control_frame_t o;
    assert(control_parse_frame(big, RT_MAX_COMMAND, RT_MAX_COMMAND, &o) == 0);
    big[n - 1] = 'x';                 /* one byte over, still valid JSON if it were read */
    big[n++] = '"';
    big[n++] = '}';
    assert(control_parse_frame(big, (size_t)n, RT_MAX_COMMAND, &o) == -1);

    ok("{\"seq\":4294967295,\"t\":-1.00,\"y\":-1.00}", -1.0f, -1.0f);

    /* --- malformed ----------------------------------------------------------- */
    bad("abc");
    bad("{\"t\":0.5}");            // one axis without the other
    bad("{\"y\":0.5}");
    bad("{}");
    bad("");
    bad(NULL);
    bad("{\"t\":nan,\"y\":0}");    // non-finite rejected
    bad("{\"t\":inf,\"y\":0}");
    bad("{\"t\":1,\"y\":-inf}");
    bad("{\"t\"0.5,\"y\":0}");     // missing colon
    bad("{\"t\":\"0.5\",\"y\":0}");// string value, not a JSON number
    bad("{\"t\":1e,\"y\":0}");     // half a number
    bad("{\"seq\":-1,\"t\":0,\"y\":0}");            // seq is unsigned on the wire
    bad("{\"seq\":99999999999,\"t\":0,\"y\":0}");   // and fits 32 bits
    /* One past UINT32_MAX, in ten digits so the token length rule does not catch it
       first. unsigned long is 64 bits on this host and 32 on the car, so without the
       ERANGE check this frame was rejected here and accepted there. */
    bad("{\"seq\":4294967296,\"t\":0,\"y\":0}");
    ok("{\"seq\":4294967295,\"t\":0,\"y\":0}", 0.0f, 0.0f);   // the last legal one
    /* Only a real key position counts: the name inside a string value must not match. */
    bad("{\"note\":\"t\":0,\"y\":0}");

    /* Nothing is read past `len`, so a buffer that is not NUL-terminated is safe. */
    const char raw[] = "{\"t\":0.5,\"y\":0}{\"t\":1,\"y\":1}";
    control_frame_t part;
    assert(control_parse_frame(raw, 16, RT_MAX_COMMAND, &part) == 0);
    assert(part.has_ty && approx(part.t, 0.5f) && approx(part.y, 0.0f));

    printf("test_control_proto: all passed\n");
    return 0;
}
