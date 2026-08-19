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

#ifndef RAMP_HOST_TEST
#include "esp_err.h"
// Start the 50 Hz ramp task (sole PCA9685 writer). Loads ramp_ms from NVS (default 300).
esp_err_t ramp_init(void);
// Set the 8-channel duty target (copied under the ramp lock).
void ramp_set_target(const uint16_t duty[8]);
// Acceleration time 0→full in ms (0 = ramp off / instant). Clamped to 0..2000. Persisted by caller.
void ramp_set_ms(uint16_t ms);
uint16_t ramp_get_ms(void);
// Persist the current ramp_ms as a JSON string in NVS.
void ramp_save(void);
#endif
#endif // RAMP_H
