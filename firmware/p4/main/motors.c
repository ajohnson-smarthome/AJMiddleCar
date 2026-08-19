#include "motors.h"

static float absf(float x) { return x < 0.0f ? -x : x; }

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
        uint16_t duty = (uint16_t)(mag * 4095.0f + 0.5f);

        if (s > cfg->deadzone) {          // forward
            out.duty[ch_a] = duty;
            out.duty[ch_b] = 0;
        } else if (s < -cfg->deadzone) {  // reverse
            out.duty[ch_a] = 0;
            out.duty[ch_b] = duty;
        } else {                          // stop
            out.duty[ch_a] = 0;
            out.duty[ch_b] = 0;
        }
    }
    return out;
}
