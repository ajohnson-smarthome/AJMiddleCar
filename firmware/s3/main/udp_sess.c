#include "udp_sess.h"

void udp_sess_init(udp_sess_table_t *t)
{
    for (int i = 0; i < UDP_SESS_MAX; i++) {
        t->s[i].addr = 0;
        t->s[i].port = 0;
        t->s[i].last_ms = 0;
        t->s[i].used = false;
    }
}

int udp_sess_find(const udp_sess_table_t *t, uint32_t addr, uint16_t port)
{
    for (int i = 0; i < UDP_SESS_MAX; i++) {
        if (t->s[i].used && t->s[i].addr == addr && t->s[i].port == port) {
            return i;
        }
    }
    return -1;
}

int udp_sess_touch(udp_sess_table_t *t, uint32_t addr, uint16_t port, uint32_t now_ms)
{
    int idx = udp_sess_find(t, addr, port);
    if (idx < 0) {
        /* A free slot first, so an eviction only ever happens once every slot is spoken for. */
        for (int i = 0; i < UDP_SESS_MAX; i++) {
            if (!t->s[i].used) {
                idx = i;
                break;
            }
        }
        if (idx < 0) {
            /* Every slot is used: evict the one silent longest — the smallest last_ms. Ties
             * resolve to the lowest index, which is arbitrary but deterministic. */
            idx = 0;
            for (int i = 1; i < UDP_SESS_MAX; i++) {
                if (t->s[i].last_ms < t->s[idx].last_ms) idx = i;
            }
        }
        t->s[idx].used = true;
        t->s[idx].addr = addr;
        t->s[idx].port = port;
    }
    t->s[idx].last_ms = now_ms;
    return idx;
}

uint32_t udp_sess_expire(udp_sess_table_t *t, uint32_t now_ms, uint32_t idle_ms)
{
    uint32_t freed = 0;
    for (int i = 0; i < UDP_SESS_MAX; i++) {
        /* Unsigned subtraction rather than a signed comparison: a millisecond counter that
         * wraps at 2^32 still elapses correctly this way — the same idiom as the car's
         * watchdog_stale (firmware/p4/main/watchdog.h). */
        if (t->s[i].used && (uint32_t)(now_ms - t->s[i].last_ms) > idle_ms) {
            t->s[i].used = false;
            freed |= (1u << i);
        }
    }
    return freed;
}
