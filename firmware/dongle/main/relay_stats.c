#include "relay_stats.h"

#include <string.h>

/* Nothing from either relay's header reaches this file, which is what keeps it free of
 * ESP-IDF and host-testable with plain cc. */
static relay_stats_t s_shared;

relay_stats_t *relay_stats_shared(void)
{
    return &s_shared;
}

void relay_stats_init(relay_stats_t *s)
{
    memset(s, 0, sizeof(*s));
}

void relay_stats_forwarded(relay_stats_t *s, bool to_car)
{
    if (to_car) s->total_to_car++;
    else        s->total_to_phone++;
}

void relay_stats_failed(relay_stats_t *s, int err, uint32_t now_ms)
{
    if (err == s->last_errno) {
        s->errno_count++;
    } else {
        s->last_errno = err;
        s->errno_count = 1;
    }
    s->last_fail_ms = now_ms;
}

void relay_stats_udp_slots(relay_stats_t *s, uint8_t used) { s->udp_used = used; }
void relay_stats_tcp_slots(relay_stats_t *s, uint8_t used) { s->tcp_used = used; }

/* x10 packets per second, in integer arithmetic: counted * 10 * 1000 / elapsed_ms.
 * The multiply is done in uint64_t because counted * 10000 overflows uint32_t above
 * ~429k packets in one window, which a long window at full rate can reach. */
static uint16_t rate_x10(uint32_t counted, uint32_t elapsed_ms)
{
    uint64_t r = ((uint64_t)counted * 10000u) / elapsed_ms;
    return r > 0xFFFFu ? 0xFFFFu : (uint16_t)r;
}

void relay_stats_sample(relay_stats_t *s, uint32_t now_ms)
{
    uint32_t elapsed = now_ms - s->mark_ms;   /* unsigned: correct across a wrap */
    if (elapsed == 0) return;                 /* no time passed; keep the last reading */
    s->to_car_x10   = rate_x10(s->total_to_car   - s->mark_to_car,   elapsed);
    s->to_phone_x10 = rate_x10(s->total_to_phone - s->mark_to_phone, elapsed);
    s->mark_to_car   = s->total_to_car;
    s->mark_to_phone = s->total_to_phone;
    s->mark_ms = now_ms;
}
