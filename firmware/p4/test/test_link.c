#define RAMP_HOST_TEST
#define LINK_HOST_TEST
#include "../main/link.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Telemetry's "ctl" is a closed vocabulary the app switches on. The names come from
   the schema through link.h; this is the check that every source has one and that no
   two share it — a duplicate would report the wrong owner, and a missing one would
   send "?" to a phone that has no case for it. */
static void ctl_vocabulary(void) {
    const link_src_t all[] = { LINK_SRC_NONE, LINK_SRC_RECOVER, LINK_SRC_CONSOLE,
                               LINK_SRC_RT, LINK_SRC_CALIB, LINK_SRC_OTA, LINK_SRC_SAFE };
    const int n = (int)(sizeof(all) / sizeof(all[0]));
    assert(n == CTL_COUNT);
    for (int i = 0; i < n; i++) {
        assert(strcmp(link_src_name(all[i]), "?") != 0);
        for (int j = i + 1; j < n; j++) {
            assert(strcmp(link_src_name(all[i]), link_src_name(all[j])) != 0);
        }
    }
    assert(strcmp(link_src_name(LINK_SRC_NONE), CTL_NONE) == 0);
    assert(strcmp(link_src_name(LINK_SRC_RT), CTL_RT) == 0);
    assert(strcmp(link_src_name(LINK_SRC_SAFE), CTL_SAFE) == 0);
    assert(strcmp(link_src_name((link_src_t)99), "?") == 0);
}

int main(void) {
    ctl_vocabulary();

    /* The RT grant must outlive the watchdog deadline by one actuator tick: with the
       two equal, the grant's >= lapsed the target to zero up to a tick before the
       trip's > declared the loss, so every trip began from motors already at rest —
       and a frame arriving exactly on the deadline dipped the duty with no trip at
       all. link.h's own comment claimed this ordering could not happen. */
    assert(LINK_HOLD_RT_MS == (uint32_t)RT_WATCHDOG_MS + LINK_TICK_MS);

    link_arb_t a = { .owner = LINK_SRC_NONE, .until_ms = 0, .sticky = false };

    /* Nobody owns it: anyone may take it. */
    assert(link_arb_lapsed(&a, 0));
    assert(link_arb_grant(&a, LINK_SRC_RT, 1000, 300, false));
    assert(a.owner == LINK_SRC_RT);
    assert(!link_arb_lapsed(&a, 1000));
    assert(!link_arb_lapsed(&a, 1299));
    assert(link_arb_lapsed(&a, 1300));       /* the grant lapses exactly on its deadline */

    /* A live owner refuses anything below it, and refusing does not disturb the grant. */
    assert(!link_arb_grant(&a, LINK_SRC_CONSOLE, 1100, 0, true));
    assert(!link_arb_grant(&a, LINK_SRC_RECOVER, 1100, 0, true));
    assert(a.owner == LINK_SRC_RT);
    assert(a.until_ms == 1300);

    /* Equal rank refreshes: this is the 10 Hz stream holding its own grant open. */
    assert(link_arb_grant(&a, LINK_SRC_RT, 1200, 300, false));
    assert(a.until_ms == 1500);

    /* Higher rank pre-empts. */
    assert(link_arb_grant(&a, LINK_SRC_CALIB, 1250, 600, false));
    assert(a.owner == LINK_SRC_CALIB);
    assert(link_arb_grant(&a, LINK_SRC_SAFE, 1260, 0, true));
    assert(a.owner == LINK_SRC_SAFE);

    /* Sticky ownership never lapses on time. */
    assert(!link_arb_lapsed(&a, 1260));
    assert(!link_arb_lapsed(&a, 0xFFFFFFFFu));

    /* Release frees it, and only for the source that holds it. */
    link_arb_release(&a, LINK_SRC_RT);              /* not the owner — ignored */
    assert(a.owner == LINK_SRC_SAFE);
    link_arb_release(&a, LINK_SRC_SAFE);
    assert(a.owner == LINK_SRC_NONE);
    assert(link_arb_lapsed(&a, 1260));

    /* Once lapsed, the lowest source may take it. */
    a = (link_arb_t){ .owner = LINK_SRC_RT, .until_ms = 1300, .sticky = false };
    assert(link_arb_grant(&a, LINK_SRC_RECOVER, 1300, 0, true));
    assert(a.owner == LINK_SRC_RECOVER);

    /* Millisecond-counter rollover: a deadline just past UINT32_MAX still expires
       in order, the same way recovery_evict and watchdog_stale handle it. */
    a = (link_arb_t){ .owner = LINK_SRC_RT, .until_ms = 0xFFFFFF00u + 300,
                      .sticky = false };
    assert(!link_arb_lapsed(&a, 0xFFFFFF00u));      /* before the deadline */
    assert(link_arb_lapsed(&a, 0x00000100u));       /* wrapped past it */

    /* --- write ordering: within a pair, the fall lands before the rise -----------
       A single ascending pass wrote a reversal's rising channel while its pair-mate
       still held the old duty on the chip — both BTS7960 inputs driven for the I2C
       gap, and for >=20 ms per tick while the fall's write kept failing. */
    {
        uint16_t cur[8] = { 2000, 0, 0, 1500, 0, 0, 0, 0 };
        uint16_t tgt[8] = { 0, 4095, 0, 1500, 0, 0, 300, 0 };
        uint16_t next[8];
        uint8_t  order[8];
        uint8_t  n = link_plan_writes(cur, tgt, 4095, next, order);
        assert(n == 3);
        assert(order[0] == 0);                    /* the fall (ch0: 2000 -> 0) first */
        assert(order[1] == 1 && order[2] == 6);   /* rises after, ascending */
        assert(next[0] == 0 && next[1] == 4095 && next[6] == 300);
        assert(next[3] == 1500);                  /* unchanged channel: no write */

        /* Bounded rise still ramps; fall is instant. */
        uint16_t cur2[8] = { 0, 1000, 0, 0, 0, 0, 0, 0 };
        uint16_t tgt2[8] = { 500, 0, 0, 0, 0, 0, 0, 0 };
        n = link_plan_writes(cur2, tgt2, 100, next, order);
        assert(n == 2 && order[0] == 1 && order[1] == 0);
        assert(next[1] == 0 && next[0] == 100);

        /* At boot the shadow is SHADOW-unknown (0xFFFF): everything "falls" to its
           target, so the zeroing writes are ordered first by construction. */
        uint16_t cur3[8] = { 0xFFFF, 0xFFFF, 0, 0, 0, 0, 0, 0 };
        uint16_t tgt3[8] = { 0 };
        n = link_plan_writes(cur3, tgt3, 4095, next, order);
        assert(n == 2 && order[0] == 0 && order[1] == 1 && next[0] == 0);
    }
    /* A rise may not land while the pair-mate holds ANY duty on the chip — the
       unknown boot shadow counts as driving. */
    assert(link_rise_safe(0, 4095));
    assert(link_rise_safe(2000, 0));      /* writing a zero is always safe */
    assert(!link_rise_safe(2000, 4095));
    assert(!link_rise_safe(0xFFFF, 1));

    printf("test_link: all passed\n");
    return 0;
}
