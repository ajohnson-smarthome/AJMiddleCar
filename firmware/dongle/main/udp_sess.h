#ifndef UDP_SESS_H
#define UDP_SESS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Which phone is on the other end of a relayed real-time session, and when it was last heard
 * from. Pure: no sockets, no ESP-IDF, and time arrives as an argument, so the eviction and
 * expiry rules are host-tested instead of observed on a bench during a drive.
 *
 * The table is small on purpose. One phone drives one car; the extra slots exist so that a
 * phone which reconnects from a new source port does not have to wait out the old session's
 * timeout before it can drive. */
#define UDP_SESS_MAX 4

/* Ten seconds. The app streams commands at 10 Hz and the car answers at 5 Hz, so a live session
 * is never quiet for more than a fraction of a second; anything silent this long has ended. Long
 * enough that a stalled phone does not lose its slot mid-drive, short enough that a table of
 * four cannot be exhausted by abandoned sessions in any realistic session. */
#define UDP_SESS_IDLE_MS 10000u

typedef struct {
    uint32_t addr;     /* the phone's address, network byte order */
    uint16_t port;     /* the phone's source port, host byte order */
    uint32_t last_ms;
    bool     used;
} udp_sess_t;

typedef struct {
    udp_sess_t s[UDP_SESS_MAX];
} udp_sess_table_t;

void udp_sess_init(udp_sess_table_t *t);

/* The index of this peer's session, creating it if new and evicting the least recently used
 * slot when the table is full. Never fails: a relay that refuses a packet because its table is
 * full would drop a drive rather than an abandoned session. */
int udp_sess_touch(udp_sess_table_t *t, uint32_t addr, uint16_t port, uint32_t now_ms);

/* The index of this peer's session, or -1. Does not create and does not update the deadline. */
int udp_sess_find(const udp_sess_table_t *t, uint32_t addr, uint16_t port);

/* Free every slot silent for longer than idle_ms. Returns a bitmask of the freed indices so the
 * caller can close exactly those sockets — the pure module names no socket, and the caller
 * needs to know which of its own to close. */
uint32_t udp_sess_expire(udp_sess_table_t *t, uint32_t now_ms, uint32_t idle_ms);

#endif /* UDP_SESS_H */
