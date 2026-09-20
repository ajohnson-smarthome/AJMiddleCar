#include "battery_soc.h"
#include <string.h>
#include "battery_pack.h"

// All charge arithmetic is in mA·ms: a 200 ms reading at 5 A is 1 000 000 of them, the whole
// pack is 3.24e10 — past uint32, hence int64 throughout.
#define MAMS_PER_MAH   (3600LL * 1000)
#define CAPACITY_MAMS  ((int64_t)BATTERY_CAPACITY_MAH * MAMS_PER_MAH)
#define START_MS       ((uint32_t)BATTERY_START_S * 1000)
#define REST_MS        ((uint32_t)BATTERY_REST_S * 1000)
// One percent per second, per millisecond: capacity / 100 / 1000.
#define PULL_MAMS_PER_MS ((int64_t)BATTERY_CAPACITY_MAH * 36)

static const battery_rest_point_t s_rest[BATTERY_REST_POINTS] = BATTERY_REST_TABLE;

void battery_soc_reset(battery_soc_t *s) { memset(s, 0, sizeof(*s)); }
void battery_soc_init(battery_soc_t *s) { battery_soc_reset(s); }

int battery_soc_rest_pct(int32_t mv_per_cell) {
    if (mv_per_cell >= s_rest[0].mv) return s_rest[0].pct;
    for (int i = 1; i < BATTERY_REST_POINTS; i++) {
        const battery_rest_point_t *hi = &s_rest[i - 1], *lo = &s_rest[i];
        if (mv_per_cell < lo->mv) continue;
        // lo.mv <= mv < hi.mv: linear between the two, to the nearest percent
        int32_t num = (mv_per_cell - lo->mv) * (hi->pct - lo->pct);
        int32_t den = hi->mv - lo->mv;
        return lo->pct + (num + den / 2) / den;
    }
    return s_rest[BATTERY_REST_POINTS - 1].pct;
}

static int64_t pct_to_mams(int pct) { return CAPACITY_MAMS * pct / 100; }

static int mams_to_pct(int64_t mams) {
    return (int)((mams * 100 + CAPACITY_MAMS / 2) / CAPACITY_MAMS);
}

int battery_soc_step(battery_soc_t *s, int32_t mv, int32_t ma, uint32_t dt_ms) {
    int at_rest = ma < BATTERY_REST_MA && ma > -BATTERY_REST_MA;
    if (!at_rest) {
        s->rest_ms = 0;
        s->start_mv_ms = 0;
    } else if (s->rest_ms < REST_MS) {
        s->rest_ms += dt_ms;             // saturates once the pull-in is due: never wraps
    }

    if (!s->known) {
        if (!at_rest) return -1;
        s->start_mv_ms += (int64_t)mv * dt_ms;
        if (s->rest_ms < START_MS) return -1;
        // Time-weighted average of the window's readings — the two sums ran over the same ticks
        int32_t avg_mv = (int32_t)(s->start_mv_ms / s->rest_ms);
        s->charge_mams = pct_to_mams(battery_soc_rest_pct(avg_mv / BATTERY_CELLS));
        s->known = 1;
        return mams_to_pct(s->charge_mams);
    }

    // Coulombs: discharge positive. The count itself is clamped, not just the percent —
    // an over-discharged count is not a debt the charger has to pay off first.
    s->charge_mams -= (int64_t)ma * dt_ms;
    if (s->charge_mams < 0) s->charge_mams = 0;
    if (s->charge_mams > CAPACITY_MAMS) s->charge_mams = CAPACITY_MAMS;

    // At rest long enough, pull toward what the rest voltage says — at most 1 %/s, and no
    // further than the table's number. After this tick's coulombs, so an idle draw does
    // not leave the count one notch below the table between ticks.
    if (s->rest_ms >= REST_MS) {
        int64_t target = pct_to_mams(battery_soc_rest_pct(mv / BATTERY_CELLS));
        int64_t max_step = PULL_MAMS_PER_MS * dt_ms;
        int64_t gap = target - s->charge_mams;
        if (gap > max_step) gap = max_step;
        if (gap < -max_step) gap = -max_step;
        s->charge_mams += gap;
    }
    return mams_to_pct(s->charge_mams);
}
