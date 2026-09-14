#include "../main/uplink.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    uplink_t u;
    uplink_init(&u);
    assert(!uplink_dead(&u));

    /* A healthy link: every send is answered, the strike count never climbs. */
    for (unsigned i = 0; i < 200; i++) {
        assert(!uplink_sent(&u, 1000 + i));
        uplink_heard(&u);
    }
    assert(!uplink_dead(&u));

    /* Sends that succeed and are never answered are strikes, the same as sends that fail:
     * one short of the streak is not dead, and asks for nothing. */
    for (unsigned i = 0; i < UPLINK_DEAD_AFTER - 1; i++) assert(!uplink_sent(&u, 2000 + i));
    assert(!uplink_dead(&u));
    /* The streak's last strike is the one that asks for a reassociation — once — and the
     * kick restarts the count: the re-joined association gets a whole streak to answer in. */
    assert(uplink_failed(&u, 2100));
    assert(!uplink_dead(&u));
    /* Strikes keep coming while the radio is still reassociating: dead again after twenty,
     * but no second kick inside the spacing, however long the streak grows, whichever kind. */
    for (unsigned i = 0; i < 100; i++) assert(!uplink_sent(&u, 2101 + i));
    for (unsigned i = 0; i < 100; i++) assert(!uplink_failed(&u, 2201 + i));
    assert(uplink_dead(&u));
    /* After the spacing a link that is still dead is asked for again. */
    assert(uplink_sent(&u, 2100 + UPLINK_KICK_SPACING_MS));
    assert(!uplink_dead(&u));
    /* One datagram from the car ends the streak and the verdict. */
    uplink_heard(&u);
    assert(!uplink_dead(&u));
    /* And the next streak counts from one — the spacing gates the kick, not the count. */
    for (unsigned i = 0; i < UPLINK_DEAD_AFTER - 1; i++) assert(!uplink_failed(&u, 30000 + i));
    assert(uplink_failed(&u, 30100));

    /* A streak that completes inside the spacing of the previous kick waits it out. */
    uplink_init(&u);
    for (unsigned i = 0; i < UPLINK_DEAD_AFTER; i++) uplink_sent(&u, 100 + i);   /* kicked at 119 */
    uplink_heard(&u);
    for (unsigned i = 0; i < UPLINK_DEAD_AFTER - 1; i++) assert(!uplink_sent(&u, 500 + i));
    assert(!uplink_sent(&u, 600));                         /* dead, but too soon to kick again */
    assert(uplink_dead(&u));
    assert(uplink_sent(&u, 119 + UPLINK_KICK_SPACING_MS));
    assert(!uplink_dead(&u));

    /* The clock wrapping is not a spacing. */
    uplink_init(&u);
    for (unsigned i = 0; i < UPLINK_DEAD_AFTER - 1; i++) uplink_sent(&u, 0xFFFFFFF0u);
    assert(uplink_sent(&u, 0xFFFFFFF0u));                       /* kicked; the count restarts */
    for (unsigned i = 0; i < UPLINK_DEAD_AFTER; i++) assert(!uplink_sent(&u, 0xFFFFFFF0u + 100));
    assert(uplink_dead(&u));                                     /* dead again, inside the spacing */
    assert(uplink_sent(&u, 0xFFFFFFF0u + UPLINK_KICK_SPACING_MS));
    printf("uplink: ok\n");
    return 0;
}
