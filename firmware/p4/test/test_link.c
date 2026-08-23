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

/* One actuator tick over the pure planners, the way link_task runs them: kick, plan,
   then apply the ordered writes to the chip shadow behind the rise_safe gate — with
   the shoot-through invariant checked after every landed write. */
static void tick(uint16_t cur[8], const uint16_t cmd[8], uint8_t kick[8],
                 uint16_t next[8]) {
    uint16_t tgt[8], up[8];
    uint8_t  order[8];
    memcpy(tgt, cmd, 8 * sizeof(uint16_t));
    link_kick_plan(cur, tgt, up, kick, RU, KD, (uint8_t)KT);
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

    /* --- the start kick: a channel leaving standstill on a small command ---------
       Stiction crutch (board.h): a stopped brushed motor under load ignores small
       duty, so the first KT ticks run at KD with the ramp bypassed, then the duty
       falls — instantly, falls are never ramped — to what was commanded. */
    {
        uint16_t cur[8] = {0}, next[8], cmd[8] = {0};
        uint8_t  kick[8] = {0};

        /* Standstill + small target: KD for exactly KT ticks, then the command. */
        cmd[0] = 1200;                    /* below KD -> eligible */
        tick(cur, cmd, kick, next);
        assert(next[0] == KD);            /* tick 1: straight to KD, ramp bypassed (RU=273) */
        assert(cur[0] == KD && kick[0] == KT - 1);
        tick(cur, cmd, kick, next);
        assert(next[0] == KD);            /* tick 2: held */
        tick(cur, cmd, kick, next);
        assert(next[0] == KD && kick[0] == 0);   /* tick 3: last kick tick */
        tick(cur, cmd, kick, next);
        assert(next[0] == 1200 && cur[0] == 1200);  /* tick 4: instant fall to the command */
        tick(cur, cmd, kick, next);
        assert(next[0] == 1200 && kick[0] == 0);    /* moving now — never re-kicked */

        /* Target 0 mid-kick: the stop lands this tick and the kick state clears. */
        memset(cur, 0, sizeof(cur)); memset(kick, 0, sizeof(kick));
        cmd[0] = 1200;
        tick(cur, cmd, kick, next);
        assert(cur[0] == KD && kick[0] == KT - 1);
        cmd[0] = 0;
        tick(cur, cmd, kick, next);
        assert(next[0] == 0 && cur[0] == 0 && kick[0] == 0);

        /* An already-moving channel never kicks: it just ramps. */
        memset(cur, 0, sizeof(cur)); memset(kick, 0, sizeof(kick));
        cur[0] = 500;
        cmd[0] = 1200;
        tick(cur, cmd, kick, next);
        assert(kick[0] == 0 && next[0] == 500 + RU);

        /* A target at or above KD needs no assist: no kick, normal ramp from zero. */
        memset(cur, 0, sizeof(cur)); memset(kick, 0, sizeof(kick));
        cmd[0] = 3000;
        tick(cur, cmd, kick, next);
        assert(kick[0] == 0 && next[0] == RU);
        memset(cur, 0, sizeof(cur));
        cmd[0] = KD;                      /* the boundary itself: < is strict */
        tick(cur, cmd, kick, next);
        assert(kick[0] == 0 && next[0] == RU);
    }

    /* --- a kicked rise still waits for its pair-mate's fall ----------------------
       Reversal under kick: ch1 held 2000 on the chip, the new command is a small
       forward on ch0 — kicked to KD. The fall is ordered first; while its write
       keeps failing, rise_safe defers the kick, and at no written state are both
       bridge inputs nonzero. */
    {
        uint16_t chip[8] = { 0, 2000, 0, 0, 0, 0, 0, 0 };   /* the shadow: chip truth */
        uint16_t cmd[8]  = { 1200, 0, 0, 0, 0, 0, 0, 0 };
        uint8_t  kick[8] = {0};
        uint16_t tgt[8], up[8], next[8];
        uint8_t  order[8];

        /* Tick 1, and ch1's fall write FAILS (the shadow keeps the old duty). */
        memcpy(tgt, cmd, sizeof(tgt));
        link_kick_plan(chip, tgt, up, kick, RU, KD, (uint8_t)KT);
        assert(tgt[0] == KD && up[0] == 4095 && kick[0] == KT - 1);
        uint8_t n = link_plan_writes(chip, tgt, up, next, order);
        assert(n == 2 && order[0] == 1 && order[1] == 0);   /* mate's fall first */
        /* order[0]: the write fails — chip[1] stays 2000. order[1]: the kicked rise
           must now be refused by the gate, exactly as link_task's loop refuses it. */
        assert(!link_rise_safe(chip[0 ^ 1], next[0]));
        assert(!(chip[0] && chip[1]));                      /* still only ch1 driven */

        /* Tick 2, and the fall now lands: zero first, then the kick — never both. */
        memcpy(tgt, cmd, sizeof(tgt));
        link_kick_plan(chip, tgt, up, kick, RU, KD, (uint8_t)KT);
        assert(kick[0] == KT - 2 && tgt[0] == KD);          /* still kicking, no re-arm */
        n = link_plan_writes(chip, tgt, up, next, order);
        assert(n == 2 && order[0] == 1 && order[1] == 0);
        for (uint8_t k = 0; k < n; k++) {
            uint8_t ch = order[k];
            if (!link_rise_safe(chip[ch ^ 1], next[ch])) continue;
            chip[ch] = next[ch];
            assert(!(chip[0] && chip[1]));                  /* after every landed write */
        }
        assert(chip[1] == 0 && chip[0] == KD);              /* reversed, kick applied */
    }

    printf("test_link: all passed\n");
    return 0;
}
