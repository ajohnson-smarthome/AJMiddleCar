#include "motors.h"
#include <assert.h>
#include <stdio.h>

// Default calibration: FL->pair0, FR->pair1, RL->pair2, RR->pair3, no inversion.
static motors_config_t default_cfg(void) {
    motors_config_t c = {
        .wheels = {
            [POS_FL] = { .channel_pair = 0, .sign = 1 },
            [POS_FR] = { .channel_pair = 1, .sign = 1 },
            [POS_RL] = { .channel_pair = 2, .sign = 1 },
            [POS_RR] = { .channel_pair = 3, .sign = 1 },
        },
        .deadzone = 0.05f,
    };
    return c;
}

static void expect(const char *name, uint16_t got, uint16_t want) {
    if (got != want) {
        printf("FAIL %s: got %u, want %u\n", name, got, want);
        assert(0);
    }
}

int main(void) {
    motors_config_t cfg = default_cfg();

    // Straight full: all CH_A=4095, CH_B=0.
    motor_outputs_t o = motors_plan(1.0f, 1.0f, &cfg);
    expect("fwd ch0", o.duty[0], 4095); expect("fwd ch1", o.duty[1], 0);
    expect("fwd ch2", o.duty[2], 4095); expect("fwd ch3", o.duty[3], 0);
    expect("fwd ch4", o.duty[4], 4095); expect("fwd ch5", o.duty[5], 0);
    expect("fwd ch6", o.duty[6], 4095); expect("fwd ch7", o.duty[7], 0);

    // Stop: all zero.
    o = motors_plan(0.0f, 0.0f, &cfg);
    for (int i = 0; i < 8; i++) expect("stop", o.duty[i], 0);

    // Full reverse: both sides -1 -> all CH_A=0, CH_B=4095.
    o = motors_plan(-1.0f, -1.0f, &cfg);
    expect("rev ch0", o.duty[0], 0); expect("rev ch1", o.duty[1], 4095);
    expect("rev ch2", o.duty[2], 0); expect("rev ch3", o.duty[3], 4095);
    expect("rev ch4", o.duty[4], 0); expect("rev ch5", o.duty[5], 4095);
    expect("rev ch6", o.duty[6], 0); expect("rev ch7", o.duty[7], 4095);

    // Tank (left=+1, right=-1): left side forward (CH_A), right side reverse (CH_B).
    o = motors_plan(1.0f, -1.0f, &cfg);
    expect("tank FL A", o.duty[0], 4095); expect("tank FL B", o.duty[1], 0);
    expect("tank RL A", o.duty[4], 4095); expect("tank RL B", o.duty[5], 0);
    expect("tank FR A", o.duty[2], 0);    expect("tank FR B", o.duty[3], 4095);
    expect("tank RR A", o.duty[6], 0);    expect("tank RR B", o.duty[7], 4095);

    // Half throttle: duty ~ 2048.
    o = motors_plan(0.5f, 0.5f, &cfg);
    expect("half ch0", o.duty[0], 2048); expect("half ch1", o.duty[1], 0);
    expect("half ch2", o.duty[2], 2048); expect("half ch3", o.duty[3], 0);
    expect("half ch4", o.duty[4], 2048); expect("half ch5", o.duty[5], 0);
    expect("half ch6", o.duty[6], 2048); expect("half ch7", o.duty[7], 0);

    // Deadzone: tiny speed -> stop.
    o = motors_plan(0.02f, 0.0f, &cfg);
    for (int i = 0; i < 8; i++) expect("deadzone", o.duty[i], 0);

    // Deadzone boundary: speed == deadzone (0.05) -> stop (branch is strict '>').
    o = motors_plan(0.05f, 0.05f, &cfg);
    for (int i = 0; i < 8; i++) expect("deadzone boundary", o.duty[i], 0);

    // Sign inversion on FL: left=+1 -> FL goes reverse (CH_B), not forward.
    cfg.wheels[POS_FL].sign = -1;
    o = motors_plan(1.0f, 0.0f, &cfg);
    expect("rev FL A", o.duty[0], 0); expect("rev FL B", o.duty[1], 4095);
    expect("rev RL A (unchanged)", o.duty[4], 4095);

    // Right-side inversion: invert FR (pair1). Drive right side forward, left zero.
    cfg = default_cfg();
    cfg.wheels[POS_FR].sign = -1;
    o = motors_plan(0.0f, 1.0f, &cfg);   // left=0 -> FL,RL stop; right=1
    expect("rev FR A", o.duty[2], 0);    expect("rev FR B", o.duty[3], 4095); // FR reversed -> CH_B
    expect("RR A (unchanged)", o.duty[6], 4095); expect("RR B", o.duty[7], 0); // RR sign +1 -> forward
    expect("FL stop A", o.duty[0], 0);   expect("FL stop B", o.duty[1], 0);    // left=0 -> stop

    printf("test_motors: all passed\n");
    return 0;
}
