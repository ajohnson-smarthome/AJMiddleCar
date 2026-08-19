#ifndef WHEEL_H
#define WHEEL_H

#include <stdint.h>
#include <stdbool.h>

// Param bounds (validated by wheel_set + the /wheel API).
#define WHEEL_D_MIN_MM        20
#define WHEEL_D_MAX_MM        150
#define WHEEL_PPR_MIN         1
#define WHEEL_PPR_MAX         1000
#define WHEEL_GEAR_X100_MIN   100      // 1:1
#define WHEEL_GEAR_X100_MAX   30000    // 1:300

// Wheel + encoder geometry. gear_x100 = gear ratio × 100 (1:21 → 2100; 1:9.6 → 960).
// quad = quadrature edge multiplier (1, 2, or 4). diameter in mm.
typedef struct {
    uint16_t diameter_mm;
    uint16_t ppr;        // encoder pulses per motor-shaft revolution (one channel)
    uint16_t gear_x100;
    uint8_t  quad;
} wheel_params_t;

// Load params from NVS (or defaults: 65 mm, 11 PPR, 1:21, ×4). Call once at boot.
void wheel_init(void);
// Copy current params out.
void wheel_get(wheel_params_t *out);
// Validate/clamp and store in RAM (the /wheel API persists to NVS).
void wheel_set(const wheel_params_t *in);
// Serialize the current params to a JSON string and persist to NVS (one key).
void wheel_save(void);

// Pure (host-tested): counts per OUTPUT-shaft revolution = ppr × gear × quad.
// Laid in for the future on-board speed calc (v = π·D·ticks_per_s / cpr); unused for now.
static inline float wheel_cpr(const wheel_params_t *w) {
    return (float)w->ppr * ((float)w->gear_x100 / 100.0f) * (float)w->quad;
}

#endif // WHEEL_H
