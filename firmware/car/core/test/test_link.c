#define RAMP_HOST_TEST
#define LINK_HOST_TEST
#include "../main/link.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Telemetry's "owner" is a closed vocabulary the app switches on. The names come from
   the schema through link.h; this is the check that every source has one and that no
   two share it — a duplicate would report the wrong owner, and a missing one would
   send "?" to a phone that has no case for it. */
static void owner_vocabulary(void) {
    const link_src_t all[] = { LINK_SRC_NONE, LINK_SRC_RECOVER, LINK_SRC_CONSOLE,
                               LINK_SRC_RT, LINK_SRC_CALIB, LINK_SRC_OTA, LINK_SRC_SAFE };
    const int n = (int)(sizeof(all) / sizeof(all[0]));
    assert(n == MOTORS_OWNER_COUNT);
    for (int i = 0; i < n; i++) {
        assert(strcmp(link_src_name(all[i]), "?") != 0);
        for (int j = i + 1; j < n; j++) {
            assert(strcmp(link_src_name(all[i]), link_src_name(all[j])) != 0);
        }
    }
    assert(strcmp(link_src_name(LINK_SRC_NONE), MOTORS_OWNER_IDLE) == 0);
    assert(strcmp(link_src_name(LINK_SRC_RECOVER), MOTORS_OWNER_RECOVERING) == 0);
    assert(strcmp(link_src_name(LINK_SRC_RT), MOTORS_OWNER_REMOTE) == 0);
    assert(strcmp(link_src_name(LINK_SRC_CALIB), MOTORS_OWNER_CALIBRATION) == 0);
    assert(strcmp(link_src_name(LINK_SRC_OTA), MOTORS_OWNER_UPDATE) == 0);
    assert(strcmp(link_src_name(LINK_SRC_SAFE), MOTORS_OWNER_SAFE_STOP) == 0);
    assert(strcmp(link_src_name((link_src_t)99), "?") == 0);
}

/* The wizard's pulse is not a drive. LINK_HOLD_CALIB_MS at 1600 (~40 %) under the slowest
   ramp never reached its duty: at rise_ms 2000 the channel climbs 40 a tick, 30 ticks make
   1200, and the grant lapses (AJM-109). The plan for the calibration owner is unramped;
   every other owner ramps by ramp.rise_ms as before. */
static void calib_outside_the_ramp(void) {
    assert(link_max_up(LINK_SRC_CALIB, 2000, LINK_TICK_MS) == 4095);
    assert(link_max_up(LINK_SRC_CALIB, 300, LINK_TICK_MS) == 4095);
    assert(link_max_up(LINK_SRC_RT, 2000, LINK_TICK_MS) == 40);
    assert(link_max_up(LINK_SRC_RECOVER, 300, LINK_TICK_MS) == 273);
    assert(link_max_up(LINK_SRC_CONSOLE, 0, LINK_TICK_MS) == 4095);   /* ramp off: no limit */
    assert(link_max_up(LINK_SRC_NONE, 2000, LINK_TICK_MS) == 40);

    /* The arithmetic of the finding, as the planner sees it: one tick to the pulse's duty
       for calibration; the remote stream at the same rise_ms would take 40 ticks. */
    uint16_t cur[8] = { 0 }, tgt[8] = { 1600, 0, 0, 0, 0, 0, 0, 0 }, next[8];
    uint8_t  order[8];
    assert(link_plan_writes(cur, tgt, link_max_up(LINK_SRC_CALIB, 2000, LINK_TICK_MS),
                            next, order) == 1);
    assert(next[0] == 1600);
    assert(link_plan_writes(cur, tgt, link_max_up(LINK_SRC_RT, 2000, LINK_TICK_MS),
                            next, order) == 1);
    assert(next[0] == 40);
}

/* A recorder for the tick's effects table: what the boards saw, in order. */
typedef struct {
    char     log[64][16];      /* "recover", "zero", "init", "pwm:ch=duty" */
    int      n;
    bool     ready;            /* pca9685_ready() */
    bool     init_ok;          /* what the next init attempt answers */
    int      pwm_fail_ch;      /* a channel whose writes fail, or -1 */
    uint32_t now;
} rec_t;

static void rec_push(rec_t *r, const char *s) {
    assert(r->n < 64);
    strncpy(r->log[r->n], s, sizeof(r->log[r->n]) - 1);
    r->log[r->n][sizeof(r->log[r->n]) - 1] = 0;
    r->n++;
}
static bool     rec_ready(void *c)       { return ((rec_t *)c)->ready; }
static void     rec_zero_all(void *c)    { rec_push((rec_t *)c, "zero"); }
static bool     rec_init(void *c)        { rec_t *r = c; rec_push(r, "init"); r->ready = r->init_ok; return r->ready; }
static void     rec_recover(void *c)     { rec_push((rec_t *)c, "recover"); }
static uint32_t rec_now(void *c)         { return ((rec_t *)c)->now; }
static bool     rec_set_pwm(void *c, uint8_t ch, uint16_t duty) {
    rec_t *r = c;
    char s[16];
    snprintf(s, sizeof(s), "pwm:%u=%u", ch, duty);
    rec_push(r, s);
    return (int)ch != r->pwm_fail_ch;
}
static const link_tick_fx_t rec_fx = {
    .ctx = NULL, .ready = rec_ready, .zero_all = rec_zero_all, .init = rec_init,
    .set_pwm = rec_set_pwm, .bus_recover = rec_recover, .now_ms = rec_now,
};
static link_tick_fx_t rec_bind(rec_t *r) { link_tick_fx_t fx = rec_fx; fx.ctx = r; return fx; }

/* Boot with the boards down (app_main's init failed): every retry from the writer clocks the
   bus free first, zeroes, then inits — paced by the clock, once a second — and a retry that
   brings the boards up leaves the shadows UNKNOWN, so the next tick writes eight zeros and
   the bus word turns ok on that evidence, with no command from anyone (AJM-127, AJM-91). */
static void tick_brings_the_boards_up(void) {
    rec_t r = { .ready = false, .init_ok = false, .pwm_fail_ch = -1, .now = 100 };
    link_tick_fx_t fx = rec_bind(&r);
    link_tick_t t;
    link_tick_init(&t);
    for (int ch = 0; ch < 8; ch++) assert(t.cur[ch] == LINK_SHADOW_UNKNOWN);
    const uint16_t zeros[8] = { 0 };

    /* First tick after boot: an attempt at once, the bus reset before it. */
    assert(link_tick_io(&t, zeros, 273, &fx) == LINK_TICK_STILL_DOWN);
    assert(r.n == 3);
    assert(strcmp(r.log[0], "recover") == 0);
    assert(strcmp(r.log[1], "zero") == 0);
    assert(strcmp(r.log[2], "init") == 0);

    /* Inside the second: nothing, not even a write. */
    r.now = 600;
    assert(link_tick_io(&t, zeros, 273, &fx) == LINK_TICK_WAITING);
    assert(r.n == 3);

    /* A second later: the same sequence, and this time the boards come up. */
    r.now = 1100;
    r.init_ok = true;
    assert(link_tick_io(&t, zeros, 273, &fx) == LINK_TICK_CAME_UP);
    assert(r.n == 6);
    assert(strcmp(r.log[3], "recover") == 0);
    assert(strcmp(r.log[4], "zero") == 0);
    assert(strcmp(r.log[5], "init") == 0);
    for (int ch = 0; ch < 8; ch++) assert(t.cur[ch] == LINK_SHADOW_UNKNOWN);   /* not zero */

    /* The next tick, target still all zero: eight writes of zero, and only then is the bus
       ok. Before the fix the shadows were zeroed with the init, the planner had nothing to
       write, and bus_ok stayed false until the first nonzero command. */
    r.now = 1120;
    assert(link_tick_io(&t, zeros, 273, &fx) == LINK_TICK_WROTE);
    assert(r.n == 14);
    for (int ch = 0; ch < 8; ch++) {
        char want[16];
        snprintf(want, sizeof(want), "pwm:%d=0", ch);
        assert(strcmp(r.log[6 + ch], want) == 0);
        assert(t.cur[ch] == 0);
    }

    /* Standing still on a healthy bus: nothing to write, the word is left alone. */
    r.now = 1140;
    assert(link_tick_io(&t, zeros, 273, &fx) == LINK_TICK_IDLE);
    assert(r.n == 14);
}

/* The write path's reset, unchanged: a failing write marks its channel unknown and the tick
   FAILED; failing for about a second earns a bus reset, then one more each second. */
static void tick_resets_a_wedged_bus(void) {
    rec_t r = { .ready = true, .init_ok = true, .pwm_fail_ch = 2, .now = 5000 };
    link_tick_fx_t fx = rec_bind(&r);
    link_tick_t t;
    link_tick_init(&t);
    for (int ch = 0; ch < 8; ch++) t.cur[ch] = 0;
    uint16_t tgt[8] = { 0, 0, 1000, 0, 0, 0, 0, 0 };

    assert(link_tick_io(&t, tgt, 4095, &fx) == LINK_TICK_FAILED);
    assert(r.n == 1 && strcmp(r.log[0], "pwm:2=1000") == 0);
    assert(t.cur[2] == LINK_SHADOW_UNKNOWN);

    /* Still failing, but not for a second yet: no reset. Ramps from zero on the retry. */
    r.now = 5500;
    assert(link_tick_io(&t, tgt, 273, &fx) == LINK_TICK_FAILED);
    assert(r.n == 2 && strcmp(r.log[1], "pwm:2=273") == 0);

    /* A second of failure: the bus is clocked free after this tick's failed write. */
    r.now = 6000;
    assert(link_tick_io(&t, tgt, 273, &fx) == LINK_TICK_RESET);
    assert(r.n == 4 && strcmp(r.log[2], "pwm:2=273") == 0 && strcmp(r.log[3], "recover") == 0);

    /* The write lands: the channel follows the chip, the failure run is over. */
    r.pwm_fail_ch = -1;
    r.now = 6020;
    assert(link_tick_io(&t, tgt, 273, &fx) == LINK_TICK_WROTE);
    assert(t.cur[2] == 273);
}

int main(void) {
    owner_vocabulary();

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

        /* An unknown shadow is not a value to ramp FROM. link.c marks a channel unknown when
           a write fails, and ramp_step(0xFFFF, tgt) is an instant fall to tgt — so a single
           NACK mid-ramp made the retry land the whole target in one tick, 273 -> 4095 with no
           slew. The plan ramps from zero instead: the slowest possible rise, which is also the
           only one that is safe when the chip's real duty is unknown. */
        uint16_t cur4[8] = { LINK_SHADOW_UNKNOWN, 0, 0, 0, 0, 0, 0, 0 };
        uint16_t tgt4[8] = { 4095, 0, 0, 0, 0, 0, 0, 0 };
        n = link_plan_writes(cur4, tgt4, 273, next, order);
        assert(n == 1 && order[0] == 0);
        assert(next[0] == 273);   /* not 4095 */
    }
    /* A rise may not land while the pair-mate holds ANY duty on the chip — the
       unknown boot shadow counts as driving. */
    assert(link_rise_safe(0, 4095));
    assert(link_rise_safe(2000, 0));      /* writing a zero is always safe */
    assert(!link_rise_safe(2000, 4095));
    assert(!link_rise_safe(0xFFFF, 1));

    calib_outside_the_ramp();
    tick_brings_the_boards_up();
    tick_resets_a_wedged_bus();

    printf("test_link: all passed\n");
    return 0;
}
