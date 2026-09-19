#include "uplink.h"
#include <stdatomic.h>

static uplink_t s_shared;

uplink_t *uplink_shared(void) { return &s_shared; }

void uplink_init(uplink_t *u)
{
    atomic_store(&u->silent_since_ms, 0u);
    atomic_store(&u->streak_ms, 0u);
    atomic_store(&u->last_kick_ms, 0u);
}

void uplink_heard(uplink_t *u)
{
    /* Length first, then the start: the other order leaves a moment with no streak and a dead
     * length, and a reader that lands in it would call an uplink the car just spoke on dead. */
    atomic_store(&u->streak_ms, 0u);
    atomic_store(&u->silent_since_ms, 0u);
}

bool uplink_dead(const uplink_t *u)
{
    /* Both words, because a strike racing a heard can leave the length standing for the
     * instant before the strike's own reset lands — no streak, no verdict. */
    return atomic_load(&u->silent_since_ms) != 0u &&
           atomic_load(&u->streak_ms) >= UPLINK_DEAD_AFTER_MS;
}

/* Starts a streak at `stamp`: an empty length, then the start, so no reader sees the new start
 * with the old length. */
static void restart(uplink_t *u, uint32_t stamp)
{
    atomic_store(&u->streak_ms, 0u);
    atomic_store(&u->silent_since_ms, stamp);
}

static bool strike(uplink_t *u, uint32_t now_ms)
{
    uint32_t stamp = now_ms != 0u ? now_ms : 1u;   /* 0 means "no streak", so a send at t=0 stamps 1 */
    uint32_t since = atomic_load(&u->silent_since_ms);
    uint32_t len   = atomic_load(&u->streak_ms);
    /* No streak yet — or the phone itself has been quiet for a threshold since the streak's
     * latest send, in which case nobody was asking and the car was not failing to answer
     * (uplink.h: `bye` gets nothing back by design). Either way this send is where the
     * measurement starts. Unsigned differences throughout: the millisecond counter's wrap
     * elapses correctly, the same idiom as udp_sess_expire. */
    if (since == 0u || (uint32_t)(stamp - (since + len)) >= UPLINK_DEAD_AFTER_MS) {
        restart(u, stamp);
        return false;
    }
    len = (uint32_t)(stamp - since);
    atomic_store(&u->streak_ms, len);
    if (len < UPLINK_DEAD_AFTER_MS) return false;
    /* Dead. Kick unless one is already in flight: `last` is what this task saw, and the
     * exchange fails for every task but the first to see a kick due — they return false and
     * leave the radio to the one that won. Signed difference, so the millisecond counter's
     * wrap is not read as ten seconds having passed. */
    uint32_t last = atomic_load(&u->last_kick_ms);
    if (last != 0u && (int32_t)(now_ms - last) < (int32_t)UPLINK_KICK_SPACING_MS) return false;
    if (!atomic_compare_exchange_strong(&u->last_kick_ms, &last, stamp)) return false;
    /* The kick's own grace: the fresh association gets a whole threshold to answer in, rather
     * than being kicked again by the first send after a re-join that took longer than the
     * spacing — which would be a station re-joining forever with nothing ever heard. */
    atomic_store(&u->streak_ms, 0u);
    atomic_store(&u->silent_since_ms, 0u);
    return true;
}

bool uplink_sent(uplink_t *u, uint32_t now_ms)   { return strike(u, now_ms); }
bool uplink_failed(uplink_t *u, uint32_t now_ms) { return strike(u, now_ms); }
