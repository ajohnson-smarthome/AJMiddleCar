#include "uplink.h"
#include <stdatomic.h>

static uplink_t s_shared;

uplink_t *uplink_shared(void) { return &s_shared; }

void uplink_init(uplink_t *u)
{
    atomic_store(&u->strikes, 0u);
    atomic_store(&u->last_kick_ms, 0u);
}

void uplink_heard(uplink_t *u)
{
    atomic_store(&u->strikes, 0u);
}

bool uplink_dead(const uplink_t *u)
{
    return atomic_load(&u->strikes) >= UPLINK_DEAD_AFTER;
}

static bool strike(uplink_t *u, uint32_t now_ms)
{
    /* Saturating: a link that stays dead for days must not wrap the streak back to zero. */
    uint32_t n = atomic_load(&u->strikes);
    while (n < UINT32_MAX && !atomic_compare_exchange_weak(&u->strikes, &n, n + 1u)) {}
    if (n + 1u < UPLINK_DEAD_AFTER) return false;
    /* Dead. Kick unless one is already in flight: `last` is what this task saw, and the
     * exchange fails for every task but the first to see a kick due — they return false and
     * leave the radio to the one that won. Signed difference, so the millisecond counter's
     * wrap is not read as ten seconds having passed. */
    uint32_t last = atomic_load(&u->last_kick_ms);
    if (last != 0u && (int32_t)(now_ms - last) < (int32_t)UPLINK_KICK_SPACING_MS) return false;
    uint32_t stamp = now_ms != 0u ? now_ms : 1u;   /* 0 means "never", so a kick at t=0 stamps 1 */
    return atomic_compare_exchange_strong(&u->last_kick_ms, &last, stamp);
}

bool uplink_sent(uplink_t *u, uint32_t now_ms)   { return strike(u, now_ms); }
bool uplink_failed(uplink_t *u, uint32_t now_ms) { return strike(u, now_ms); }
