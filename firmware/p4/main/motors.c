#include "motors.h"
#include "board.h"   /* BOARD_DUTY_FLOOR — all #defines, so this stays host-compilable */

static float absf(float x) { return x < 0.0f ? -x : x; }

// Command magnitude -> PWM duty. Not the old linear mag*4095: commands above the
// deadzone are remapped onto [BOARD_DUTY_FLOOR..4095], because a loaded brushed motor
// ignores small duty — it hums below the floor and never turns. The floor is a crutch
// with a hand-guessed constant (board.h) until per-wheel measurement replaces it.
// Only called with mag > dz, so k lands in (0..1]; the clamps guard float edge cases.
static uint16_t duty_for(float mag, float dz) {
    float span = 1.0f - dz;
    float k = span > 0.0f ? (mag - dz) / span : 1.0f;  // mag > dz guaranteed by the branch
    if (k > 1.0f) k = 1.0f;
    if (k < 0.0f) k = 0.0f;
    return (uint16_t)(BOARD_DUTY_FLOOR + k * (4095.0f - BOARD_DUTY_FLOOR) + 0.5f);
}

static float side_for(wheel_pos_t pos, float left, float right) {
    switch (pos) {
        case POS_FL:
        case POS_RL: return left;
        case POS_FR:
        case POS_RR: return right;
        default:     return 0.0f;
    }
}

motor_outputs_t motors_plan(float left, float right, const motors_config_t *cfg) {
    motor_outputs_t out = { .duty = {0} };

    for (int p = 0; p < POS_COUNT; p++) {
        const wheel_calib_t *w = &cfg->wheels[p];
        // Runtime guard (asserts are compiled out in release): channels index duty[8],
        // so channel_pair >= 4 would write out of bounds. Skip the wheel instead (stays 0).
        if (w->channel_pair >= 4) continue;
        float s = side_for((wheel_pos_t)p, left, right) * (float)w->sign;

        uint8_t ch_a = (uint8_t)(w->channel_pair * 2);
        uint8_t ch_b = (uint8_t)(ch_a + 1);

        float mag = absf(s);
        if (mag > 1.0f) mag = 1.0f;

        if (s > cfg->deadzone) {          // forward
            out.duty[ch_a] = duty_for(mag, cfg->deadzone);
            out.duty[ch_b] = 0;
        } else if (s < -cfg->deadzone) {  // reverse
            out.duty[ch_a] = 0;
            out.duty[ch_b] = duty_for(mag, cfg->deadzone);
        } else {                          // stop
            out.duty[ch_a] = 0;
            out.duty[ch_b] = 0;
        }
    }
    return out;
}
