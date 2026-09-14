#ifndef CONTROL_PROTO_H
#define CONTROL_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Room for the session id, NUL included. The wire uses eight hex characters; this is
// wider so a longer id is a rejected frame rather than a truncated one — two sessions
// whose ids differ only past the cut would otherwise look like the same session.
#define CONTROL_SID_MAX 16

// What a datagram says it is — its `type` word. The four the app sends (view on the
// video port); the two the car sends (hello_ack, telemetry) are refused here, since a
// car does not take its own words back as instructions.
typedef enum { CT_NONE = 0, CT_HELLO, CT_DRIVE, CT_BYE, CT_VIEW } control_type_t;

// One decoded real-time datagram. `type` is what the datagram is; the flags say which
// optional keys were there. A caller that needs one and finds it absent must drop the
// frame rather than read the zero left behind.
typedef struct {
    control_type_t type;
    bool     has_proto;
    uint32_t proto;
    bool     has_seq;
    uint32_t seq;
    bool     has_axes;              // both axes were present and finite
    float    throttle, turn;
    char     sid[CONTROL_SID_MAX];  // CT_HELLO, CT_VIEW: NUL-terminated, alphanumeric, non-empty
    bool     has_key;               // CT_VIEW: the `key` flag was present
    bool     key;                   // …and asked for a keyframe
} control_frame_t;

// Parse one datagram of `len` bytes into `out`. Zero-alloc and bounded: nothing is read
// past `len`, so the buffer need not be NUL-terminated, and a datagram longer than
// `max_len` is rejected untouched (the caller passes RT_MAX_COMMAND, the largest
// datagram the car accepts — the cap belongs to the transport, so it arrives as an
// argument rather than being compiled in here).
//
// Returns 0 when the datagram is a known type carrying what that type needs — a hello
// with a session, a drive with seq and both axes, a bye with seq, a view with a session
// (and optionally a boolean key) — and every key present parsed cleanly. Returns -1:
// oversized, unparseable, no type or a type the app does not send, a missing required
// key, one axis without the other, a non-finite axis, a session id that is empty,
// over-long or not alphanumeric, or any key that appears twice. `*out` is undefined on -1.
//
// `proto` is parsed when present and never judged: whether the car speaks it is policy,
// and the classifier in rt_link.h owns it (a foreign hello is still answered).
// Range is deliberately not checked: car_drive clamps, and a parser that also enforced
// policy would have two reasons to change.
int control_parse_frame(const char *msg, size_t len, size_t max_len, control_frame_t *out);

// Pure: is `seq` newer than `last`? Signed difference of unsigned counters, so the
// uint32 wraps correctly — the alternative, `seq > last`, drops every frame for the
// rest of the session the first time the counter passes 2^32.
static inline bool control_seq_newer(uint32_t seq, uint32_t last) {
    return (int32_t)(seq - last) > 0;
}

#endif // CONTROL_PROTO_H
