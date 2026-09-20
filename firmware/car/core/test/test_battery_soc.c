// The state-of-charge rule of car/battery-monitor, scenario by scenario, in numbers: the
// start window, coulombs, the rest pull-in, the table, the bounds. The monitor task feeds
// one reading every 200 ms; so does this.
#include "../main/battery_soc.h"
#include "../main/battery_pack.h"
#include <assert.h>
#include <stdio.h>

#define TICK_MS 200
#define MAMS_PER_MAH (3600LL * 1000)

// Feed `total_ms` of identical readings, one tick at a time; return the last verdict.
static int feed(battery_soc_t *s, int32_t mv, int32_t ma, uint32_t total_ms) {
    int r = -2;
    for (uint32_t t = 0; t < total_ms; t += TICK_MS) r = battery_soc_step(s, mv, ma, TICK_MS);
    return r;
}

// Feed a discharge (or charge, negative) of `mah` at `ma`, at pack voltage `mv`.
static int feed_mah(battery_soc_t *s, int32_t mv, int32_t ma, int64_t mah) {
    int64_t ms = mah * MAMS_PER_MAH / (ma < 0 ? -ma : ma);
    assert(ms % TICK_MS == 0);            // the scenario's numbers divide into ticks
    return feed(s, mv, ma, (uint32_t)ms);
}

static void test_start_window(void) {
    battery_soc_t s;
    battery_soc_init(&s);
    // Full pack at rest: -1 for the first 2.8 s, 100 at 3 s. (Change: BATTERY_START_S, the
    // table's top point, or a window that closes on the first reading.)
    for (int i = 0; i < 14; i++) assert(battery_soc_step(&s, 12600, 300, TICK_MS) == -1);
    assert(battery_soc_step(&s, 12600, 300, TICK_MS) == 100);
    // and stays known
    assert(battery_soc_step(&s, 12600, 300, TICK_MS) == 100);
    // 12.0 V at rest is 80 (4000 mV/cell) — the start value is the table's, not a constant
    battery_soc_init(&s);
    assert(feed(&s, 12000, 0, 3000) == 80);
    // the window's average is time-weighted, so a first tick at 12.6 and then 12.0 lands
    // between — not on whichever reading came last
    battery_soc_init(&s);
    assert(feed(&s, 12600, 0, 1500) == -1);
    int r = feed(&s, 12000, 0, 1500);
    assert(r > 80 && r < 100);
    printf("  start window ok\n");
}

static void test_start_under_current(void) {
    battery_soc_t s;
    battery_soc_init(&s);
    // A pack under load for 5 s answers -1 for all of it: a sagging voltage is not a rest
    // voltage. (Change: dropping the |ma| < BATTERY_REST_MA condition.)
    assert(feed(&s, 12600, 2000, 5000) == -1);
    // Charging counts as "not at rest" too
    assert(feed(&s, 12600, -2000, 5000) == -1);
    // Rest right at the threshold is not rest; just under it is
    assert(feed(&s, 12600, BATTERY_REST_MA, 5000) == -1);
    // Rest after that closes the window — in its own three seconds, not the leftovers
    assert(feed(&s, 12600, BATTERY_REST_MA - 1, 2800) == -1);
    assert(feed(&s, 12600, BATTERY_REST_MA - 1, TICK_MS) == 100);
    // An interrupted rest reopens the window: 2 s rest, a tick of load, 2 s rest → still -1
    battery_soc_init(&s);
    assert(feed(&s, 12600, 0, 2000) == -1);
    assert(feed(&s, 12600, 2000, TICK_MS) == -1);
    assert(feed(&s, 12600, 0, 2000) == -1);
    assert(feed(&s, 12600, 0, 1000) == 100);
    printf("  start under current ok\n");
}

static void test_coulombs(void) {
    battery_soc_t s;
    battery_soc_init(&s);
    assert(feed(&s, 12000, 0, 3000) == 80);
    // 4500 mAh out of 9000, from 80: 30 — and the sagging voltage under load (10.8 V, the
    // table would say 0) plays no part. (Change: capacity, the mA·ms arithmetic, a voltage
    // term sneaking into the count.)
    assert(feed_mah(&s, 10800, 5000, 4500) == 30);
    // Charge raises it: 900 mAh back at -3 A → 40
    assert(feed_mah(&s, 12600, -3000, 900) == 40);
    // A draw of 600 mA for half an hour costs 300 mAh: 36.67 %, shown as the nearest, 37
    assert(feed_mah(&s, 11300, 600, 300) == 37);
    printf("  coulombs ok\n");
}

static void test_rest_pull(void) {
    battery_soc_t s;
    battery_soc_init(&s);
    assert(feed(&s, 12000, 0, 3000) == 80);
    assert(feed_mah(&s, 11000, 5000, 3600) == 40);     // an hour's drive
    // Now at rest, and the rest voltage says 45 (3750 mV/cell): for 30 s nothing moves
    // (Change: BATTERY_REST_S, or pulling as soon as the current drops.)
    assert(battery_soc_rest_pct(3750) == 45);
    uint32_t t;
    for (t = 0; t < (uint32_t)BATTERY_REST_S * 1000 - TICK_MS; t += TICK_MS)
        assert(battery_soc_step(&s, 11250, 0, TICK_MS) == 40);
    // The tick that completes the 30th second begins the climb: never more than 1 %/s
    // (as the nearest integer would show it), 45 by the fifth second, and it stops there.
    // (Change: the rate, the direction, running past the target.)
    for (t = 0; t < 5200; t += TICK_MS) {
        int r = battery_soc_step(&s, 11250, 0, TICK_MS);
        assert(r >= 40 && r <= 40 + (int)((t + TICK_MS + 500) / 1000));
        if (t + TICK_MS == 5000) assert(r == 45);
    }
    assert(feed(&s, 11250, 0, 10000) == 45);
    // With the idle draw of a real car (300 mA) it still holds the table's number, not
    // one below it: the pull comes after the tick's coulombs, not before
    assert(feed(&s, 11250, 300, 10000) == 45);
    // The pull works downward too: a rest voltage of 3720 (40) brings 45 back to 40
    assert(feed(&s, 11160, 0, 1000) == 44);
    assert(feed(&s, 11160, 0, 4000) == 40);
    assert(feed(&s, 11160, 0, 5000) == 40);
    // Load ends the rest: 90 mAh out → 39, and the next pull needs its 30 s of rest again
    assert(feed_mah(&s, 10800, 5000, 90) == 39);
    assert(feed(&s, 11250, 0, (uint32_t)BATTERY_REST_S * 1000 - TICK_MS) == 39);
    assert(feed(&s, 11250, 0, 3 * TICK_MS) == 40);            // 39 + 3 × 0.2, rounded
    printf("  rest pull ok\n");
}

static void test_table(void) {
    // The points themselves
    assert(battery_soc_rest_pct(4200) == 100);
    assert(battery_soc_rest_pct(4100) == 90);
    assert(battery_soc_rest_pct(3600) == 20);
    assert(battery_soc_rest_pct(3400) == 5);
    assert(battery_soc_rest_pct(3200) == 0);
    // Between points: linear. (Change: snapping to the nearest point, or the wrong pair.)
    assert(battery_soc_rest_pct(4150) == 95);
    assert(battery_soc_rest_pct(3960) == 75);
    assert(battery_soc_rest_pct(3550) == 15);
    assert(battery_soc_rest_pct(3300) == 3);     // 2.5, to the nearest
    // Outside: clamped, never extrapolated
    assert(battery_soc_rest_pct(4300) == 100);
    assert(battery_soc_rest_pct(3000) == 0);
    assert(battery_soc_rest_pct(0) == 0);
    assert(battery_soc_rest_pct(-1) == 0);
    // The table is the pack's, and it is what the rule reads
    battery_rest_point_t table[BATTERY_REST_POINTS] = BATTERY_REST_TABLE;
    for (int i = 0; i < BATTERY_REST_POINTS; i++) {
        assert(battery_soc_rest_pct(table[i].mv) == table[i].pct);
        if (i) assert(table[i].mv < table[i - 1].mv && table[i].pct < table[i - 1].pct);
    }
    printf("  table ok\n");
}

static void test_bounds(void) {
    battery_soc_t s;
    battery_soc_init(&s);
    assert(feed(&s, 12000, 0, 3000) == 80);
    // 20000 mAh out of a 9000 pack: 0, and it stays 0
    assert(feed_mah(&s, 10000, 5000, 20000) == 0);
    assert(feed_mah(&s, 10000, 5000, 900) == 0);
    // The count clamps, not just the reading: 900 mAh back in reads 10, not "still owed"
    // (Change: clamping the returned percent while the count runs negative.)
    assert(feed_mah(&s, 12000, -3000, 900) == 10);
    // Over the top the same way: 20000 mAh in → 100, and the next 900 out → 90
    assert(feed_mah(&s, 12600, -3000, 20000) == 100);
    assert(feed_mah(&s, 12600, -3000, 900) == 100);
    assert(feed_mah(&s, 12000, 5000, 900) == 90);
    // A zero dt is a no-op, not a division by zero
    battery_soc_init(&s);
    assert(battery_soc_step(&s, 12600, 0, 0) == -1);
    assert(feed(&s, 12600, 0, 3000) == 100);
    assert(battery_soc_step(&s, 12600, 0, 0) == 100);
    printf("  bounds ok\n");
}

static void test_reset(void) {
    battery_soc_t s;
    battery_soc_init(&s);
    assert(feed(&s, 12000, 0, 3000) == 80);
    assert(feed_mah(&s, 11000, 5000, 1800) == 60);
    // Back from `absent`: the count is gone, the window is open again, and it closes on
    // the new rest voltage — not on the old count
    battery_soc_reset(&s);
    assert(feed(&s, 12600, 0, 2800) == -1);
    assert(feed(&s, 12600, 0, TICK_MS) == 100);
    printf("  reset ok\n");
}

int main(void) {
    test_start_window();
    test_start_under_current();
    test_coulombs();
    test_rest_pull();
    test_table();
    test_bounds();
    test_reset();
    printf("test_battery_soc: all passed\n");
    return 0;
}
