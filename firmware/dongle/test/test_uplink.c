#include "../main/uplink.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    uplink_t u;
    uplink_init(&u);
    assert(!uplink_dead(&u));

    /* One short of the streak is not dead, and asks for nothing. */
    for (unsigned i = 0; i < UPLINK_DEAD_AFTER - 1; i++) assert(!uplink_failed(&u, 1000 + i));
    assert(!uplink_dead(&u));
    /* The streak's last failure is the one that asks for a reassociation — once. */
    assert(uplink_failed(&u, 2000));
    assert(uplink_dead(&u));
    /* Failures keep coming while the radio is still reassociating: no second kick inside
     * the spacing, however long the streak grows. */
    for (unsigned i = 0; i < 100; i++) assert(!uplink_failed(&u, 2001 + i));
    assert(uplink_dead(&u));
    /* After the spacing a link that is still dead is asked for again. */
    assert(uplink_failed(&u, 2000 + UPLINK_KICK_SPACING_MS));
    /* One successful send ends the streak and the verdict. */
    uplink_sent(&u);
    assert(!uplink_dead(&u));
    /* And the next streak counts from one — the spacing gates the kick, not the count. */
    for (unsigned i = 0; i < UPLINK_DEAD_AFTER - 1; i++) assert(!uplink_failed(&u, 30000 + i));
    assert(uplink_failed(&u, 30100));

    /* A streak that completes inside the spacing of the previous kick waits it out. */
    uplink_init(&u);
    for (unsigned i = 0; i < UPLINK_DEAD_AFTER; i++) uplink_failed(&u, 100 + i);   /* kicked at 100+19 */
    uplink_sent(&u);
    for (unsigned i = 0; i < UPLINK_DEAD_AFTER - 1; i++) assert(!uplink_failed(&u, 500 + i));
    assert(!uplink_failed(&u, 600));                       /* dead, but too soon to kick again */
    assert(uplink_dead(&u));
    assert(uplink_failed(&u, 119 + UPLINK_KICK_SPACING_MS));

    /* The clock wrapping is not a spacing. */
    uplink_init(&u);
    for (unsigned i = 0; i < UPLINK_DEAD_AFTER - 1; i++) uplink_failed(&u, 0xFFFFFFF0u);
    assert(uplink_failed(&u, 0xFFFFFFF0u));
    assert(!uplink_failed(&u, 0xFFFFFFF0u + 100));
    assert(uplink_failed(&u, 0xFFFFFFF0u + UPLINK_KICK_SPACING_MS));
    printf("uplink: ok\n");
    return 0;
}
