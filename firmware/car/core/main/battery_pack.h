#ifndef BATTERY_PACK_H
#define BATTERY_PACK_H
#include <stdint.h>

// Everything about the pack itself — what the cells are, how many, what they hold — as
// opposed to board.h, which is about the board the pack powers. Not /config: a pack is
// swapped with a soldering iron, not from the pult. The bench corrects these (bringup.md).
//
// Pack: 3S3P LG HG2 (18650, NMC), confirmed 2026-09-20.
#define BATTERY_CELLS         3      // in series: pack mV / 3 = cell mV
#define BATTERY_PARALLEL      3      // in parallel: informational, the capacity below already sums them
#define BATTERY_CAPACITY_MAH  9000

// `low` is a threshold with hysteresis: in at 20, out only at 23 (car/battery-monitor).
#define BATTERY_LOW_PCT        20
#define BATTERY_LOW_CLEAR_PCT  23

// The state-of-charge rule (battery_soc.h): "at rest" is |current| under BATTERY_REST_MA;
// the start window averages BATTERY_START_S of rest voltage; after BATTERY_REST_S of rest
// the coulomb count is pulled back toward the rest table.
#define BATTERY_REST_MA    500
#define BATTERY_REST_S     30
#define BATTERY_START_S    3

// Rest voltage per cell → percent, Li-ion NMC, descending. A typical curve to start with —
// task 3.2 of ajm-178 measures the HG2 pack at four levels and corrects the points here.
typedef struct {
    uint16_t mv;
    uint8_t  pct;
} battery_rest_point_t;

#define BATTERY_REST_POINTS 12
#define BATTERY_REST_TABLE {                                                    \
    {4200, 100}, {4100, 90}, {4000, 80}, {3920, 70}, {3850, 60}, {3780, 50},    \
    {3720, 40},  {3660, 30}, {3600, 20}, {3500, 10}, {3400, 5},  {3200, 0},     \
}

#endif // BATTERY_PACK_H
