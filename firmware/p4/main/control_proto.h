#ifndef CONTROL_PROTO_H
#define CONTROL_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Room for the session id, NUL included. The wire uses eight hex characters; this is
// wider so a longer id is a rejected frame rather than a truncated one — two sessions
// whose ids differ only past the cut would otherwise look like the same session.
#define CONTROL_SID_MAX 16

// One decoded real-time datagram. Every key is optional on the wire, so each carries a
// flag saying whether it was there; a caller that needs one and finds it absent must
// drop the frame rather than read the zero left behind.
typedef struct {
    bool     has_proto;
    uint32_t proto;
    bool     has_seq;
    uint32_t seq;
    bool     has_ty;              // both axes were present and finite
    float    t, y;
    bool     has_hello;
    char     sid[CONTROL_SID_MAX];  // NUL-terminated, alphanumeric, non-empty
    bool     bye;                 // a goodbye: stop, do not retreat, drop the session
} control_frame_t;

// Parse one datagram of `len` bytes into `out`. Zero-alloc and bounded: nothing is read
// past `len`, so the buffer need not be NUL-terminated, and a datagram longer than
// `max_len` is rejected untouched (the caller passes RT_MAX_COMMAND, the largest
// datagram the car accepts — the cap belongs to the transport, so it arrives as an
// argument rather than being compiled in here).
//
// Returns 0 when the datagram carries at least one thing worth acting on — a hello, a
// goodbye, or both control axes — and every key present parsed cleanly. Returns -1:
// oversized, unparseable, one axis without the other, a non-finite axis, or a hello
// whose id is empty, over-long or not alphanumeric. `*out` is undefined on -1.
//
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
