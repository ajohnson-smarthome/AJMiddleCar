#ifndef BATTERY_SOC_H
#define BATTERY_SOC_H
#include <stdint.h>

// State of charge without a memory (car/battery-monitor): the start window reads the rest
// voltage off the table, coulombs carry the count from there, and a long rest pulls it
// back toward the table. Nothing is stored — every boot begins from the rest voltage.
// Pure, zero ESP-IDF deps — host-tested by test_battery_soc.c; the constants are
// battery_pack.h's. The monitor task (battery.c) feeds it one reading at a time.
typedef struct {
    uint32_t rest_ms;        // consecutive time at rest (|ma| < BATTERY_REST_MA), saturating
    int64_t  start_mv_ms;    // Σ mv·dt over the start window, for its time-weighted average
    int64_t  charge_mams;    // what is left, in mA·ms; 0..capacity once known
    int      known;          // the start window closed: charge_mams means something
} battery_soc_t;

void battery_soc_init(battery_soc_t *s);
// A monitor back from `absent` starts over, as after a boot.
void battery_soc_reset(battery_soc_t *s);

// The rest table, linearly interpolated between its points; clamped to 0..100 outside.
int battery_soc_rest_pct(int32_t mv_per_cell);

// One reading: pack voltage in mV, current in mA (discharge positive, charge negative),
// dt_ms since the previous reading. Returns the percent, 0..100, or -1 while the start
// window has not closed — three seconds of rest, uninterrupted: a reading under current
// reopens the window, since a sagging or recovering voltage is not a rest voltage.
int battery_soc_step(battery_soc_t *s, int32_t mv, int32_t ma, uint32_t dt_ms);

#endif // BATTERY_SOC_H
