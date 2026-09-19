#include "../main/uplink.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

/* Streams unanswered sends at `period_ms` from `from_ms` up to and including `until_ms`, and
 * returns the time of the first kick — 0 when none came. `failed` picks the spelling; the two
 * are one rule and the tests alternate them to say so. */
static uint32_t stream_until_kick(uplink_t *u, uint32_t from_ms, uint32_t period_ms,
                                  uint32_t until_ms, bool failed)
{
    for (uint32_t t = from_ms; (uint32_t)(until_ms - t) < 0x80000000u; t += period_ms) {
        bool kick = failed ? uplink_failed(u, t) : uplink_sent(u, t);
        if (kick) return t;
    }
    return 0;
}

static void test_a_live_link_never_accumulates(void)
{
    uplink_t u;
    uplink_init(&u);
    assert(!uplink_dead(&u));
    /* Control at 10 Hz for a minute, every send answered by the car's telemetry. */
    for (uint32_t t = 0; t < 60000; t += 100) {
        assert(!uplink_sent(&u, t));
        uplink_heard(&u);
    }
    assert(!uplink_dead(&u));
    /* REST polls every 3.5 s for a minute — the launch ladder's cadence — each one answered. */
    for (uint32_t t = 60000; t < 120000; t += 3500) {
        assert(!uplink_failed(&u, t));
        uplink_heard(&u);
    }
    assert(!uplink_dead(&u));
}

/* Датаграммы: серия — время от первой неотвеченной отправки, не число отправок. */
static void test_datagrams_measure_time_not_sends(void)
{
    uplink_t u;
    uplink_init(&u);
    /* 10 Hz from t=1000, nothing heard: 89 sends inside the threshold ask for nothing. */
    assert(stream_until_kick(&u, 1000, 100, 1000 + UPLINK_DEAD_AFTER_MS - 100, false) == 0);
    assert(!uplink_dead(&u));
    /* The first send at or past the threshold is the one that asks — once — and the kick
     * restarts the streak: the re-joined association gets a whole threshold to answer in. */
    assert(uplink_sent(&u, 1000 + UPLINK_DEAD_AFTER_MS));
    assert(!uplink_dead(&u));
}

/* Соединения: те же ~9 с, хотя отправок — четыре, а не девяносто. */
static void test_connections_measure_the_same_time(void)
{
    uplink_t u;
    uplink_init(&u);
    /* The ladder polls /version every ~3.5 s, and each connection the phone abandons is one
     * unanswered send at the moment it is abandoned. Under the old 20-strike rule that took
     * 70 s; the clock says dead at the first strike past the threshold. */
    uint32_t kick = stream_until_kick(&u, 1000, 3500, 1000 + 60000, true);
    assert(kick != 0);
    assert(kick - 1000 >= UPLINK_DEAD_AFTER_MS);
    assert(kick - 1000 < UPLINK_DEAD_AFTER_MS + 3500);   /* within one poll of the threshold */
    assert(!uplink_dead(&u));
}

/* Мёртвый uplink, толчок уже был: не раньше порога и не чаще интервала. */
static void test_a_kick_waits_out_the_spacing(void)
{
    uplink_t u;
    uplink_init(&u);
    uint32_t k1 = stream_until_kick(&u, 1000, 100, 60000, false);
    assert(k1 == 1000 + UPLINK_DEAD_AFTER_MS);
    /* The radio is still re-joining and nothing is heard: dead again one threshold after the
     * kick, but no second kick inside the spacing, however the streak is spelled. */
    assert(stream_until_kick(&u, k1 + 100, 100, k1 + UPLINK_KICK_SPACING_MS - 100, true) == 0);
    assert(uplink_dead(&u));
    /* After the spacing a link that is still dead is asked for again. */
    assert(uplink_sent(&u, k1 + UPLINK_KICK_SPACING_MS));
    assert(!uplink_dead(&u));
}

/* Живая связь обнуляет: одна датаграмма от машинки — и серия начинается заново. */
static void test_anything_heard_ends_the_streak(void)
{
    uplink_t u;
    uplink_init(&u);
    assert(stream_until_kick(&u, 0, 100, 5000, false) == 0);
    uplink_heard(&u);
    assert(!uplink_dead(&u));
    /* The next streak is measured from its own first send, not from the old one. */
    assert(stream_until_kick(&u, 5100, 100, 5100 + UPLINK_DEAD_AFTER_MS - 100, false) == 0);
    assert(uplink_failed(&u, 5100 + UPLINK_DEAD_AFTER_MS));
    /* A verdict that stands (dead inside the spacing) is also ended by one datagram. */
    assert(stream_until_kick(&u, 5100 + UPLINK_DEAD_AFTER_MS + 100, 100,
                             5100 + 2 * UPLINK_DEAD_AFTER_MS + 100, false) == 0);
    assert(uplink_dead(&u));
    uplink_heard(&u);
    assert(!uplink_dead(&u));
}

/* Пульт замолчал сам: старая неотвеченная отправка (`bye`) не делает мёртвым первый `hello`
 * после паузы — молчание машинки измеряется только пока её спрашивают. */
static void test_the_senders_own_pause_restarts_the_streak(void)
{
    uplink_t u;
    uplink_init(&u);
    assert(!uplink_sent(&u, 0));                      /* bye — unanswered by design */
    assert(!uplink_sent(&u, 30000));                  /* hello, half a minute later */
    assert(!uplink_dead(&u));
    /* Hello at 5 Hz with a car that really is gone: dead one threshold after the hello. */
    assert(stream_until_kick(&u, 30200, 200, 30000 + UPLINK_DEAD_AFTER_MS - 200, false) == 0);
    assert(uplink_sent(&u, 30000 + UPLINK_DEAD_AFTER_MS));

    /* A pause shorter than the threshold does not restart it: the streak still runs from its
     * first send, and the kick lands one threshold after THAT. */
    uplink_init(&u);
    const uint32_t t0 = 1000;
    assert(!uplink_failed(&u, t0));
    assert(!uplink_sent(&u, t0 + UPLINK_DEAD_AFTER_MS - 1000));
    assert(stream_until_kick(&u, t0 + UPLINK_DEAD_AFTER_MS - 800, 200,
                             t0 + UPLINK_DEAD_AFTER_MS - 200, false) == 0);
    assert(uplink_sent(&u, t0 + UPLINK_DEAD_AFTER_MS));
}

/* A streak whose first send lands on boot_ms() == 0 is still a streak. */
static void test_a_streak_can_begin_at_time_zero(void)
{
    uplink_t u;
    uplink_init(&u);
    assert(!uplink_sent(&u, 0));
    assert(stream_until_kick(&u, 100, 100, UPLINK_DEAD_AFTER_MS - 100, false) == 0);
    assert(!uplink_dead(&u));
    /* t=0 is stamped as 1 so it reads as "a streak", which costs the deadline a millisecond. */
    assert(stream_until_kick(&u, UPLINK_DEAD_AFTER_MS, 100, UPLINK_DEAD_AFTER_MS + 100, false)
           != 0);
}

/* The millisecond counter wrapping is neither an elapsed threshold nor an elapsed spacing. */
static void test_the_clock_wrap_is_not_time_passing(void)
{
    uplink_t u;
    uplink_init(&u);
    /* The first streak and its kick land before the wrap; the second streak, and the spacing
     * measured from that kick, run across it. */
    const uint32_t t0 = 0xFFFFFFF0u - UPLINK_DEAD_AFTER_MS - 5000u;
    uint32_t k1 = stream_until_kick(&u, t0, 100, t0 + 20000u, false);
    assert(k1 == t0 + UPLINK_DEAD_AFTER_MS);
    assert(!uplink_dead(&u));
    /* Dead again after the wrap, inside the spacing measured from before it: no kick yet. */
    assert(stream_until_kick(&u, k1 + 100, 100, k1 + UPLINK_KICK_SPACING_MS - 100, true) == 0);
    assert(uplink_dead(&u));
    assert(uplink_sent(&u, k1 + UPLINK_KICK_SPACING_MS));
}

int main(void)
{
    test_a_live_link_never_accumulates();
    test_datagrams_measure_time_not_sends();
    test_connections_measure_the_same_time();
    test_a_kick_waits_out_the_spacing();
    test_anything_heard_ends_the_streak();
    test_the_senders_own_pause_restarts_the_streak();
    test_a_streak_can_begin_at_time_zero();
    test_the_clock_wrap_is_not_time_passing();
    printf("uplink: ok\n");
    return 0;
}
