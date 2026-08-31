#ifndef TCP_PENDING_H
#define TCP_PENDING_H

#include <stdbool.h>

/* One direction's outbound backlog for one relay_tcp.c slot: the bytes a single non-blocking
 * send() attempt could not place, kept until relay_tcp.c's flush_pending can try again. Pure:
 * no sockets, no ESP-IDF, so the bookkeeping — stash a chunk, record partial progress, know
 * when a backlog has fully drained — is host-tested here (test_tcp_pending.c) instead of only
 * reasoned about against a live TCP stack. relay_tcp.c owns every socket and every send()/
 * recv() call; this module only tracks what is left of one, and never touches a fd.
 *
 * Sized to 1460 bytes because a backlog can never hold more than what a single relay_tcp.c
 * read produced in one shot — see relay_tcp.c's RELAY_BUF_LEN, which is this same constant. */
#define TCP_PENDING_BUF_LEN 1460

typedef struct {
    char buf[TCP_PENDING_BUF_LEN];
    int len;    /* valid bytes waiting to be sent; 0 means nothing pending for this direction */
    int off;    /* how many of those bytes have already gone out */
} tcp_pending_t;

/* Empties a backlog. Used both when a slot's connection first forms (nothing pending yet)
 * and when it closes (a future occupant of the same index must not inherit a stranger's
 * leftover bytes) — relay_tcp.c's close_slot calls this for both directions of every slot it
 * frees, unconditionally, which is what keeps that guarantee unconditional too. */
void tcp_pending_clear(tcp_pending_t *p);

/* True while there is nothing left to send for this direction. This is relay_tcp.c's whole
 * backpressure signal: its fd-set build offers a direction's SOURCE socket for reading only
 * while this is true, and its DESTINATION socket for writing only while it is false. */
bool tcp_pending_empty(const tcp_pending_t *p);

/* Records that relay_tcp.c just read a chunk of `n` bytes and placed the first `sent` of them
 * with an immediate send() attempt (0 <= sent <= n). Stashes the unsent remainder — chunk +
 * sent, for n - sent bytes — as the new backlog, replacing whatever was there before.
 *
 * Preconditions the caller must uphold (not checked here — see relay_tcp.c's pump_read, the
 * only caller): the backlog must already be empty (tcp_pending_empty(p) true) — the fd-set
 * build in relay_tcp.c guarantees this by never offering a source for reading while its
 * backlog is non-empty — and n must not exceed TCP_PENDING_BUF_LEN. */
void tcp_pending_stash(tcp_pending_t *p, const char *chunk, int n, int sent);

/* Records that flush_pending's latest send() attempt placed `w` more bytes (0 < w <= however
 * many remain). Clears the backlog to empty — the same state tcp_pending_clear leaves it in —
 * the instant every byte has gone out; otherwise just moves the internal offset forward. */
void tcp_pending_advance(tcp_pending_t *p, int w);

#endif /* TCP_PENDING_H */
