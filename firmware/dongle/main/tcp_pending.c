#include "tcp_pending.h"

#include <string.h>

void tcp_pending_clear(tcp_pending_t *p)
{
    p->len = 0;
    p->off = 0;
}

bool tcp_pending_empty(const tcp_pending_t *p)
{
    return p->len == 0;
}

void tcp_pending_stash(tcp_pending_t *p, const char *chunk, int n, int sent)
{
    int rem = n - sent;
    memcpy(p->buf, chunk + sent, (size_t)rem);
    p->len = rem;
    p->off = 0;
}

int tcp_pending_room(const tcp_pending_t *p)
{
    return TCP_PENDING_BUF_LEN - p->len;
}

void tcp_pending_append(tcp_pending_t *p, const char *chunk, int n)
{
    memcpy(p->buf + p->len, chunk, (size_t)n);
    p->len += n;
}

void tcp_pending_advance(tcp_pending_t *p, int w)
{
    p->off += w;
    /* >=, not ==. The header documents `0 < w <= however many remain` as a precondition and
       nothing checks it; on an exact match the two spellings agree, and on an overshoot only
       this one recovers. The other latched the backlog non-empty for good — relay_tcp.c's
       fd-set build would never offer that direction's source for reading again, and
       flush_pending's `len - off` would go negative and reach send() as a size_t of about four
       gigabytes out of a 1460-byte buffer. A pure, host-tested module should not have its
       bookkeeping rest on every caller getting it right. */
    if (p->off >= p->len) {
        p->len = 0;
        p->off = 0;
    }
}
