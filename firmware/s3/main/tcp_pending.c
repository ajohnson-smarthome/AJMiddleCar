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

void tcp_pending_advance(tcp_pending_t *p, int w)
{
    p->off += w;
    if (p->off == p->len) {
        p->len = 0;
        p->off = 0;
    }
}
