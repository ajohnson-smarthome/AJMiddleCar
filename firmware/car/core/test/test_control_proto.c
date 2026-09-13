#include "control_proto.h"
#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
/* The cap and the key words are the schema's, not this test's — a frame that is legal
   here and rejected on the car (or the reverse) is exactly the class of bug the
   generator exists to remove. The datagrams below are written out as wire bytes on
   purpose: a test that built them from RT_KEY_* would agree with a typo. */
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

static void drive(const char *msg, float et, float ey) {
    control_frame_t f = parse(msg);
    if (f.type != CT_DRIVE || !f.has_axes || !approx(f.throttle, et) || !approx(f.turn, ey)) {
        printf("FAIL drive('%s') -> type=%d has_axes=%d throttle=%.4f turn=%.4f (want %.4f %.4f)\n",
               msg, f.type, f.has_axes, f.throttle, f.turn, et, ey);
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
    /* --- drive, as the app sends it ------------------------------------------ */
    drive("{\"proto\":2,\"type\":\"drive\",\"seq\":1,\"throttle\":0.5,\"turn\":0}", 0.5f, 0.0f);
    drive("{\"proto\":2,\"type\":\"drive\",\"seq\":2,\"throttle\":0,\"turn\":1}", 0.0f, 1.0f);
    drive("{\"proto\":2,\"type\":\"drive\",\"seq\":3,\"throttle\":-1,\"turn\":-0.5}", -1.0f, -0.5f);
    drive("{\"turn\":-1.0,\"throttle\":1.0,\"seq\":4,\"type\":\"drive\",\"proto\":2}", 1.0f, -1.0f);   /* key order */
    drive("{ \"proto\" : 2 , \"type\" : \"drive\" , \"seq\" : 5 , \"throttle\" : 0.25 , \"turn\" : 0.75 }",
          0.25f, 0.75f);                                                                       /* whitespace */

    control_frame_t f = parse("{\"proto\":2,\"type\":\"drive\",\"seq\":1234,\"throttle\":0.50,\"turn\":-0.25}");
    assert(f.type == CT_DRIVE && f.has_seq && f.seq == 1234 && f.has_proto && f.proto == 2);
    assert(f.sid[0] == '\0');

    /* proto is carried, not judged: the classifier owns that rule. A drive without it
       parses; a drive with a foreign one parses too. */
    f = parse("{\"type\":\"drive\",\"seq\":6,\"throttle\":0,\"turn\":0}");
    assert(f.type == CT_DRIVE && !f.has_proto);
    f = parse("{\"proto\":7,\"type\":\"drive\",\"seq\":6,\"throttle\":0,\"turn\":0}");
    assert(f.has_proto && f.proto == 7);

    /* The longest legal drive fits the command cap: ten-digit seq, three-decimal axes
       with signs. 96 is the schema's cap; this is what it was sized for. */
    const char *widest = "{\"proto\":2,\"type\":\"drive\",\"seq\":4294967295,\"throttle\":-1.000,\"turn\":-1.000}";
    assert(strlen(widest) <= RT_MAX_COMMAND);
    drive(widest, -1.0f, -1.0f);

    /* --- hello ---------------------------------------------------------------- */
    f = parse("{\"proto\":2,\"type\":\"hello\",\"session\":\"7f3a91c2\"}");
    assert(f.type == CT_HELLO && strcmp(f.sid, "7f3a91c2") == 0 && f.has_proto && f.proto == 2);
    assert(!f.has_seq && !f.has_axes);
    f = parse("{\"type\":\"hello\",\"session\":\"a\"}");                     /* 1 char, no proto */
    assert(f.type == CT_HELLO && !f.has_proto && strcmp(f.sid, "a") == 0);
    f = parse("{\"proto\":2,\"type\":\"hello\",\"session\":\"abcdefghijklmno\"}");  /* 15 chars */
    assert(strcmp(f.sid, "abcdefghijklmno") == 0);
    bad("{\"proto\":2,\"type\":\"hello\",\"session\":\"abcdefghijklmnop\"}");       /* 16: refused, not cut */
    bad("{\"proto\":2,\"type\":\"hello\",\"session\":\"\"}");
    bad("{\"proto\":2,\"type\":\"hello\",\"session\":\"7f3a-91c2\"}");              /* not alphanumeric */
    bad("{\"proto\":2,\"type\":\"hello\",\"session\":12345678}");                   /* not a string */
    bad("{\"proto\":2,\"type\":\"hello\"}");                                        /* no session */
    bad("{\"proto\":2,\"type\":\"hello\",\"hello\":\"7f3a91c2\"}");                 /* the v1 key */

    /* --- bye ------------------------------------------------------------------ */
    f = parse("{\"proto\":2,\"type\":\"bye\",\"seq\":1235}");
    assert(f.type == CT_BYE && f.has_seq && f.seq == 1235 && !f.has_axes);
    /* Axes on a goodbye are tolerated (the v1 stop wrote zeros) but not required. */
    f = parse("{\"proto\":2,\"type\":\"bye\",\"seq\":1236,\"throttle\":0,\"turn\":0}");
    assert(f.type == CT_BYE && f.has_axes);
    /* Every app->car datagram except a hello carries seq — a goodbye included. One
       without it would bypass replay protection, so it is dropped rather than half-
       honoured. */
    bad("{\"proto\":2,\"type\":\"bye\"}");

    /* --- type is the discriminator, and it is required ------------------------ */
    bad("{\"proto\":2,\"seq\":1,\"throttle\":0,\"turn\":0}");                       /* no type */
    bad("{\"proto\":2,\"type\":\"drove\",\"seq\":1,\"throttle\":0,\"turn\":0}");    /* unknown */
    bad("{\"proto\":2,\"type\":\"telemetry\",\"seq\":1}");                          /* car->app only */
    bad("{\"proto\":2,\"type\":\"hello_ack\",\"session\":\"7f3a91c2\"}");           /* car->app only */
    bad("{\"proto\":2,\"type\":drive,\"seq\":1,\"throttle\":0,\"turn\":0}");        /* not a string */
    bad("{\"proto\":2,\"type\":\"drive\",\"type\":\"bye\",\"seq\":1}");             /* twice */

    /* --- a drive needs both axes and a seq ------------------------------------ */
    bad("{\"proto\":2,\"type\":\"drive\",\"seq\":1,\"throttle\":0.5}");
    bad("{\"proto\":2,\"type\":\"drive\",\"seq\":1,\"turn\":0.5}");
    bad("{\"proto\":2,\"type\":\"drive\",\"throttle\":0.5,\"turn\":0}");
    bad("{\"proto\":2,\"type\":\"drive\",\"seq\":1,\"throttle\":\"x\",\"turn\":0}");
    bad("{\"proto\":2,\"type\":\"drive\",\"seq\":1,\"throttle\":nan,\"turn\":0}");
    bad("{\"proto\":2,\"type\":\"drive\",\"seq\":1,\"throttle\":1e400,\"turn\":0}");  /* not finite */
    bad("{\"proto\":2,\"type\":\"drive\",\"seq\":-1,\"throttle\":0,\"turn\":0}");
    bad("{\"proto\":2,\"type\":\"drive\",\"seq\":1.5,\"throttle\":0,\"turn\":0}");
    bad("{\"proto\":2,\"type\":\"drive\",\"seq\":4294967296,\"throttle\":0,\"turn\":0}");
    bad("{\"proto\":1.5,\"type\":\"drive\",\"seq\":1,\"throttle\":0,\"turn\":0}");    /* proto not an int */
    /* The v1 spellings are no longer keys the car knows. */
    bad("{\"proto\":2,\"type\":\"drive\",\"seq\":1,\"t\":0.5,\"y\":0}");

    /* --- duplicated keys are two instructions in one datagram ------------------ */
    bad("{\"proto\":2,\"type\":\"drive\",\"seq\":1,\"seq\":2,\"throttle\":0,\"turn\":0}");
    bad("{\"proto\":2,\"type\":\"drive\",\"seq\":1,\"throttle\":0,\"throttle\":1,\"turn\":0}");

    /* --- not JSON, not an object, or over the cap ----------------------------- */
    bad("");
    bad(NULL);
    bad("drive");
    bad("[{\"type\":\"drive\"}]");
    bad("{\"proto\":2,\"type\":\"drive\",\"seq\":1,\"throttle\":0,\"turn\":0");     /* unterminated */
    /* Only a real key position counts: the name inside a string value must not match. */
    bad("{\"proto\":2,\"type\":\"drive\",\"seq\":1,\"note\":\"throttle\":0,\"turn\":0}");
    /* The audit's shared pinned frames: the mock pins these same bytes with these same
       outcomes, because byte-identical datagrams once drove the car and the mock apart. */
    bad("{\"proto\":2,\"type\":\"drive\",\"seq\":7,\"throttle\":.5,\"turn\":0}");      /* bare mantissa */
    bad("{\"proto\":2,\"type\":\"drive\",\"seq\":8,\"throttle\":+1,\"turn\":0}");      /* leading plus */
    bad("{\"proto\":2,\"type\":\"drive\",\"seq\":01,\"throttle\":0,\"turn\":0}");      /* leading zero */
    bad("{\"proto\":2,\"type\":\"drive\",\"seq\":12,\"throttle\":0.5x,\"turn\":0}");   /* trailing junk */
    bad("{\"proto\":2,\"type\":\"drive\",\"seq\":1,\"throttle\"0.5,\"turn\":0}");      /* missing colon */
    /* Nothing is read past `len`, so a buffer that is not NUL-terminated is safe. */
    {
        const char first[] = "{\"proto\":2,\"type\":\"drive\",\"seq\":1,\"throttle\":0.5,\"turn\":0}";
        const char raw[]   = "{\"proto\":2,\"type\":\"drive\",\"seq\":1,\"throttle\":0.5,\"turn\":0}"
                             "{\"proto\":2,\"type\":\"drive\",\"seq\":2,\"throttle\":1,\"turn\":1}";
        control_frame_t part;
        assert(control_parse_frame(raw, sizeof(first) - 1, RT_MAX_COMMAND, &part) == 0);
        assert(part.has_axes && approx(part.throttle, 0.5f) && approx(part.turn, 0.0f) && part.seq == 1);
    }
    {
        char big[RT_MAX_COMMAND + 64];
        int n = snprintf(big, sizeof(big),
                         "{\"proto\":2,\"type\":\"drive\",\"seq\":1,\"throttle\":0,\"turn\":0,\"pad\":\"");
        while (n < RT_MAX_COMMAND + 1) big[n++] = 'x';
        big[n++] = '"'; big[n++] = '}'; big[n] = '\0';
        bad(big);
    }
    /* Keys the car does not look for are ignored — but nested objects cannot smuggle one
       in: "seq" inside a sub-object is not the datagram's seq. */
    drive("{\"proto\":2,\"type\":\"drive\",\"seq\":1,\"throttle\":0.1,\"turn\":0.2,\"extra\":{\"seq\":9}}",
          0.1f, 0.2f);
    f = parse("{\"proto\":2,\"type\":\"drive\",\"seq\":1,\"throttle\":0.1,\"turn\":0.2,\"extra\":{\"seq\":9}}");
    assert(f.seq == 1);

    /* --- the sequence gate ---------------------------------------------------- */
    seq_newer(2, 1, true);
    seq_newer(1, 1, false);
    seq_newer(1, 2, false);
    seq_newer(0, 0xFFFFFFFFu, true);           /* wrap */
    seq_newer(0xFFFFFFFFu, 0, false);
    seq_newer(0x80000000u, 0, false);          /* half a ring away is "older" */
    seq_newer(0x7FFFFFFFu, 0, true);

    printf("test_control_proto: all passed\n");
    return 0;
}
