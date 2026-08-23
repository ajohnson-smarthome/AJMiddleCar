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

static void up_fill(uint16_t up[8], uint16_t v) {
    for (int ch = 0; ch < 8; ch++) up[ch] = v;
}

/* The kick constants the scenarios below use — literals, not board.h's, so retuning
   the bench guesses does not rewrite this file's arithmetic. 273 is a real ramp bound:
   ramp_max_up_per_tick(300, 20) = 4095*20/300. */
#define KD 2600u   /* kick duty */
#define KT 3u      /* kick ticks */
#define RU 273u    /* ramped rise per tick */
#define KI 10u     /* idle ticks: mirrors BOARD_KICK_IDLE_TICKS's 200 ms threshold */

/* One actuator tick over the pure planners, the way link_task runs them: kick, plan,
   then apply the ordered writes to the chip shadow behind the rise_safe gate — with
   the shoot-through invariant checked after every landed write. */
static void tick(uint16_t cur[8], const uint16_t cmd[8], uint8_t kick[8],
                 uint8_t idle[4], uint16_t next[8]) {
    uint16_t tgt[8], up[8];
    uint8_t  order[8];
    memcpy(tgt, cmd, 8 * sizeof(uint16_t));
    link_kick_plan(cur, tgt, up, kick, idle, RU, KD, (uint8_t)KT, (uint8_t)KI);
    uint8_t n = link_plan_writes(cur, tgt, up, next, order);
    for (uint8_t k = 0; k < n; k++) {
        uint8_t ch = order[k];
        if (!link_rise_safe(cur[ch ^ 1], next[ch])) continue;
        cur[ch] = next[ch];
        assert(!(cur[ch & ~1] && cur[ch | 1]));   /* never both bridge inputs driven */
    }
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
        uint16_t up[8];
        up_fill(up, 4095);
        uint16_t cur[8] = { 2000, 0, 0, 1500, 0, 0, 0, 0 };
        uint16_t tgt[8] = { 0, 4095, 0, 1500, 0, 0, 300, 0 };
        uint16_t next[8];
        uint8_t  order[8];
        uint8_t  n = link_plan_writes(cur, tgt, up, next, order);
        assert(n == 3);
        assert(order[0] == 0);                    /* the fall (ch0: 2000 -> 0) first */
        assert(order[1] == 1 && order[2] == 6);   /* rises after, ascending */
        assert(next[0] == 0 && next[1] == 4095 && next[6] == 300);
        assert(next[3] == 1500);                  /* unchanged channel: no write */

        /* Bounded rise still ramps; fall is instant. */
        uint16_t cur2[8] = { 0, 1000, 0, 0, 0, 0, 0, 0 };
        uint16_t tgt2[8] = { 500, 0, 0, 0, 0, 0, 0, 0 };
        up_fill(up, 100);
        n = link_plan_writes(cur2, tgt2, up, next, order);
        assert(n == 2 && order[0] == 1 && order[1] == 0);
        assert(next[1] == 0 && next[0] == 100);

        /* The bound is per channel now (the kick needs one channel unbounded while
           its neighbours ramp): ch0 keeps the 100 bound, ch2 rises freely. */
        uint16_t cur4[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
        uint16_t tgt4[8] = { 500, 0, 500, 0, 0, 0, 0, 0 };
        up[2] = 4095;
        n = link_plan_writes(cur4, tgt4, up, next, order);
        assert(n == 2 && next[0] == 100 && next[2] == 500);

        /* At boot the shadow is SHADOW-unknown (0xFFFF): everything "falls" to its
           target, so the zeroing writes are ordered first by construction. */
        uint16_t cur3[8] = { 0xFFFF, 0xFFFF, 0, 0, 0, 0, 0, 0 };
        uint16_t tgt3[8] = { 0 };
        up_fill(up, 4095);
        n = link_plan_writes(cur3, tgt3, up, next, order);
        assert(n == 2 && order[0] == 0 && order[1] == 1 && next[0] == 0);
    }
    /* A rise may not land while the pair-mate holds ANY duty on the chip — the
       unknown boot shadow counts as driving. */
    assert(link_rise_safe(0, 4095));
    assert(link_rise_safe(2000, 0));      /* writing a zero is always safe */
    assert(!link_rise_safe(2000, 4095));
    assert(!link_rise_safe(0xFFFF, 1));

    /* --- the start kick: a channel leaving GENUINE standstill on a small command -
       Stiction crutch (board.h): a stopped brushed motor under load ignores small
       duty, so the first KT ticks run at KD with the ramp bypassed, then the duty
       falls — instantly, falls are never ramped — to what was commanded. idle[] is
       pre-seeded to KI here: these scenarios assume the pair has already been
       sitting idle a long time before the command arrives (test 5 — the old
       scenarios all assumed standstill; the idle latch (M1) needs telling so). */
    {
        uint16_t cur[8] = {0}, next[8], cmd[8] = {0};
        uint8_t  kick[8] = {0};
        uint8_t  idle[4] = { KI, KI, KI, KI };

        /* Standstill + small target: KD for exactly KT LANDED ticks, then the
           command. m3: the counter only decrements once a landed tick shows
           cur[ch] >= kick_duty, so the arming tick itself (cur still 0 going in)
           does not decrement — kick[0] stays at KT after tick 1, and the counter
           only reaches 0 on the 4th tick, which is exactly when the fall happens.
           The COUNT of ticks driven at KD (1, 2, 3 below) is still exactly KT. */
        cmd[0] = 1200;                    /* below KD -> eligible */
        tick(cur, cmd, kick, idle, next);
        assert(next[0] == KD);            /* tick 1: straight to KD, ramp bypassed (RU=273) */
        assert(cur[0] == KD && kick[0] == KT);        /* armed, not yet decremented */
        tick(cur, cmd, kick, idle, next);
        assert(next[0] == KD && kick[0] == KT - 1);   /* tick 2: held, first landed decrement */
        tick(cur, cmd, kick, idle, next);
        assert(next[0] == KD && kick[0] == KT - 2);   /* tick 3: last kick tick */
        tick(cur, cmd, kick, idle, next);
        assert(next[0] == 1200 && cur[0] == 1200 && kick[0] == 0);  /* tick 4: instant fall */
        tick(cur, cmd, kick, idle, next);
        assert(next[0] == 1200 && kick[0] == 0);    /* moving now — never re-kicked */

        /* Target 0 mid-kick: the stop lands this tick and the kick state clears. */
        memset(cur, 0, sizeof(cur)); memset(kick, 0, sizeof(kick));
        idle[0] = KI;
        cmd[0] = 1200;
        tick(cur, cmd, kick, idle, next);
        assert(cur[0] == KD && kick[0] == KT);
        cmd[0] = 0;
        tick(cur, cmd, kick, idle, next);
        assert(next[0] == 0 && cur[0] == 0 && kick[0] == 0);

        /* An already-moving channel never kicks: it just ramps. */
        memset(cur, 0, sizeof(cur)); memset(kick, 0, sizeof(kick));
        idle[0] = KI;
        cur[0] = 500;
        cmd[0] = 1200;
        tick(cur, cmd, kick, idle, next);
        assert(kick[0] == 0 && next[0] == 500 + RU);

        /* A target at or above KD needs no assist: no kick, normal ramp from zero. */
        memset(cur, 0, sizeof(cur)); memset(kick, 0, sizeof(kick));
        idle[0] = KI;
        cmd[0] = 3000;
        tick(cur, cmd, kick, idle, next);
        assert(kick[0] == 0 && next[0] == RU);
        memset(cur, 0, sizeof(cur));
        idle[0] = KI;
        cmd[0] = KD;                      /* the boundary itself: < is strict */
        tick(cur, cmd, kick, idle, next);
        assert(kick[0] == 0 && next[0] == RU);
    }

    /* --- test 1 (M1 regression pin): a reversal never kicks ----------------------
       Pair held forward: ch0 at 2000 on the chip, never idle. The stick reverses —
       ch0 falls to 0, ch1 gets a small forward command. The OLD arm condition read
       only ch1's OWN shadow (zero, since ch1 was never driving) and armed a kick
       — an unramped KD landing against a mate whose fall is still in flight, i.e.
       plugging. The pair's idle streak is 0 here (ch0 was driving the instant
       before), so the fix must refuse the kick and just ramp ch1. */
    {
        uint16_t cur[8]  = { 2000, 0, 0, 0, 0, 0, 0, 0 };
        uint16_t cmd[8]  = { 0, 300, 0, 0, 0, 0, 0, 0 };   /* reverse: ch0->0, ch1 up */
        uint8_t  kick[8] = {0};
        uint8_t  idle[4] = {0};
        uint16_t next[8];

        tick(cur, cmd, kick, idle, next);
        assert(kick[1] == 0);            /* M1: never armed */
        assert(next[1] == RU);           /* ramped rise, NOT kick_duty — the old bug's target */
        assert(cur[1] == RU);            /* landed: ch0's fall clears the mate first */
        assert(cur[0] == 0);
    }

    /* --- test 2 (M1 regression pin): the idle latch itself ------------------------
       A pair that has JUST gone idle (idle streak below BOARD_KICK_IDLE_TICKS) must
       not kick on the next small command — only a pair that has been idle for the
       full threshold may. Old code had no idle[] at all, so it would kick the
       instant cur[ch] read zero regardless of history; this pins that the latch
       actually gates it. */
    {
        /* Below threshold: idle[0] pre-seeded one tick shy of KI. This call's own
           idle-update increments it to KI - 1, still short — no kick. */
        uint16_t cur[8] = {0}, next[8], cmd[8] = {0};
        uint8_t  kick[8] = {0};
        uint8_t  idle[4] = { KI - 2, 0, 0, 0 };
        cmd[0] = 1100;                   /* the duty-floor scale: below KD */
        tick(cur, cmd, kick, idle, next);
        assert(idle[0] == KI - 1);
        assert(kick[0] == 0 && next[0] == RU);   /* no kick: floor-duty ramped start */

        /* At/above threshold: idle[0] pre-seeded to KI already (fully idle a long
           while) — the identical small command now kicks. */
        uint16_t cur2[8] = {0}, next2[8], cmd2[8] = {0};
        uint8_t  kick2[8] = {0};
        uint8_t  idle2[4] = { KI, 0, 0, 0 };
        cmd2[0] = 1100;
        tick(cur2, cmd2, kick2, idle2, next2);
        assert(kick2[0] == KT && next2[0] == KD);   /* kicked */
    }

    /* --- test 3 (M2 regression pin): a full command mid-kick still ramps ---------
       Arm at a small command, then on the very next tick the stick goes to full
       (4095, at/above kick_duty). The bypass (up[ch] = 4095) must apply only when
       the kick ITSELF is the one rewriting the target — once the commanded target
       is already >= kick_duty, tgt is left alone and up[] keeps the ordinary ramp
       bound, so the rise from kick_duty is still bounded by RU. The old code set
       up[ch] = 4095 unconditionally whenever kick[ch] was truthy, so a stick flick
       to 4095 mid-kick jumped straight there in one tick. */
    {
        uint16_t cur[8] = {0}, next[8], cmd[8] = {0};
        uint8_t  kick[8] = {0};
        uint8_t  idle[4] = { KI, KI, KI, KI };

        cmd[0] = 1200;
        tick(cur, cmd, kick, idle, next);
        assert(next[0] == KD && cur[0] == KD);      /* tick 1: kicked to KD */

        cmd[0] = 4095;                               /* full stick, still mid-kick */
        tick(cur, cmd, kick, idle, next);
        assert(next[0] == KD + RU);                  /* ramped step from KD, NOT a jump to 4095 */
        assert(cur[0] == KD + RU);
    }

    /* --- test 4 (m3 regression pin): a deferred tick never shrinks the burst -----
       Genuine standstill, kick arms normally, but the channel's own write to the
       bus fails (simulated — pca9685_set_pwm erroring, exactly link_task's
       `failed` path) for the first two ticks: the shadow stays at 0, so the
       decrement guard (cur[ch] >= kick_duty) correctly refuses to burn the
       counter on either of them. Once the bus lets a write through, the burst
       still delivers exactly KT ticks landed at KD — counted directly against the
       simulated chip, not assumed. The old code decremented on every "kicked"
       tick regardless of whether the write landed, so the same two failures would
       have burned two-thirds of the burst before the first tick even reached the
       chip. */
    {
        uint16_t cur[8] = {0}, cmd[8] = {0};
        uint8_t  kick[8] = {0};
        uint8_t  idle[4] = { KI, KI, KI, KI };
        cmd[0] = 1200;

        uint8_t landed_at_kd = 0;
        for (uint8_t t = 0; t < 8; t++) {
            uint16_t tgt[8], up[8], nx[8];
            uint8_t  order[8];
            memcpy(tgt, cmd, sizeof(tgt));
            link_kick_plan(cur, tgt, up, kick, idle, RU, KD, (uint8_t)KT, (uint8_t)KI);
            uint8_t n = link_plan_writes(cur, tgt, up, nx, order);
            for (uint8_t k = 0; k < n; k++) {
                uint8_t ch = order[k];
                if (!link_rise_safe(cur[ch ^ 1], nx[ch])) continue;
                if (ch == 0 && t < 2) continue;   /* simulated bus failure: ticks 0,1 */
                cur[ch] = nx[ch];
            }
            if (cur[0] == KD) landed_at_kd++;
            if (kick[0] == 0 && cur[0] != KD) break;   /* burst over: fell to the command */
        }
        assert(landed_at_kd == KT);
    }

    /* --- n4: a fresh rise onto an unfallen mate never kicks, and still waits for
       the mate's fall ---------------------------------------------------------
       Formerly mislabelled "Reversal under kick": ch0 was never driving before
       this tick, so nothing here reverses — it is a fresh rise on ch0 while its
       pair-mate ch1 has not yet fallen. Per M1, that means the pair's idle streak
       is 0 (ch1 is still nonzero on the chip), so ch0 must NOT kick — it just
       ramps. The mate-ordering machinery (link_rise_safe deferring a rise behind
       its pair-mate's fall) is untouched by that fix and is exercised here too:
       ch1's fall write fails for one tick, and at no point does the shadow show
       both bridge inputs nonzero. Both asserts below are checked against writes
       the loop actually attempts this tick — a prior version of this test carried
       an `assert(!(chip[0] && chip[1]))` on a tick where the write loop never ran
       for either channel, so it could not have failed; this version runs the loop
       every tick, so the assertion is real. */
    {
        uint16_t chip[8] = { 0, 2000, 0, 0, 0, 0, 0, 0 };   /* the shadow: chip truth */
        uint16_t cmd[8]  = { 1200, 0, 0, 0, 0, 0, 0, 0 };
        uint8_t  kick[8] = {0};
        uint8_t  idle[4] = {0};                             /* nowhere near idle: ch1 drives */
        uint16_t tgt[8], up[8], next[8];
        uint8_t  order[8];

        /* Tick 1: no kick arms (M1) — ch0 just ramps. ch1's fall is still ordered
           first, and its write FAILS (simulated) this tick — the shadow keeps the
           old duty, so ch0's ramped rise is refused by rise_safe right behind it. */
        memcpy(tgt, cmd, sizeof(tgt));
        link_kick_plan(chip, tgt, up, kick, idle, RU, KD, (uint8_t)KT, (uint8_t)KI);
        assert(kick[0] == 0 && tgt[0] == 1200 && up[0] == RU);   /* M1: never armed */
        uint8_t n = link_plan_writes(chip, tgt, up, next, order);
        assert(n == 2 && order[0] == 1 && order[1] == 0);   /* mate's fall first */
        assert(next[0] == RU);                              /* ramped, not KD */
        for (uint8_t k = 0; k < n; k++) {
            uint8_t ch = order[k];
            if (ch == 1) continue;   /* simulated bus failure: ch1's fall doesn't land */
            if (!link_rise_safe(chip[ch ^ 1], next[ch])) continue;
            chip[ch] = next[ch];
            assert(!(chip[0] && chip[1]));                  /* real: this write is attempted */
        }
        assert(chip[0] == 0 && chip[1] == 2000);            /* neither write landed */

        /* Tick 2: the fall lands. ch1 -> 0, then ch0's ramped rise is finally safe. */
        memcpy(tgt, cmd, sizeof(tgt));
        link_kick_plan(chip, tgt, up, kick, idle, RU, KD, (uint8_t)KT, (uint8_t)KI);
        assert(kick[0] == 0);                               /* still never kicked */
        n = link_plan_writes(chip, tgt, up, next, order);
        assert(n == 2 && order[0] == 1 && order[1] == 0);
        for (uint8_t k = 0; k < n; k++) {
            uint8_t ch = order[k];
            if (!link_rise_safe(chip[ch ^ 1], next[ch])) continue;
            chip[ch] = next[ch];
            assert(!(chip[0] && chip[1]));                  /* after every landed write */
        }
        assert(chip[1] == 0 && chip[0] == RU);              /* ramped rise, never kicked */
    }

    printf("test_link: all passed\n");
    return 0;
}
