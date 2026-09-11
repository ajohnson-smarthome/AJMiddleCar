#ifndef RAMP_H
#define RAMP_H
#include <stdint.h>

// Pure slew-rate step: rise is limited to max_up per call, fall is instant (safe stop).
// Zero ESP-IDF deps — host-tested.
static inline uint16_t ramp_step(uint16_t current, uint16_t target, uint16_t max_up) {
    if (target <= current) return target;                 // fall (or equal): instant
    uint32_t next = (uint32_t)current + max_up;           // rise: bounded
    return next > target ? target : (uint16_t)next;
}

// Pure: how much a channel may rise in one tick, given the configured 0→full time.
// ramp_ms shorter than a tick (including 0, "off") means no limit at all.
static inline uint16_t ramp_max_up_per_tick(uint16_t ramp_ms, uint16_t tick_ms) {
    if (ramp_ms < tick_ms) return 4095;
    uint16_t step = (uint16_t)(4095u * tick_ms / ramp_ms);
    return step ? step : 1;
}

#ifndef RAMP_HOST_TEST
#include <stdbool.h>
#include "esp_err.h"
// Load ramp_ms from NVS (default 300). No task: the actuator task lives in link.c.
esp_err_t ramp_init(void);
// Acceleration time 0→full in ms (0 = ramp off / instant). Clamped to 0..2000.
// Returns false (without applying) if the internal lock could not be taken in time.
bool ramp_set_ms(uint16_t ms);
uint16_t ramp_get_ms(void);
// Persist the current ramp_ms as a JSON string in NVS, and say whether it landed.
esp_err_t ramp_save(void);
#endif
#endif // RAMP_H
