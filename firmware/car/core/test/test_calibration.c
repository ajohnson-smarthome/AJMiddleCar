#include "calibration.h"
#include <assert.h>
#include <stdio.h>

static motors_config_t good(void) {
    motors_config_t c = {
        .wheels = {
            [POS_FL] = { .channel_pair = 0, .sign = 1 },
            [POS_FR] = { .channel_pair = 1, .sign = -1 },
            [POS_RL] = { .channel_pair = 2, .sign = 1 },
            [POS_RR] = { .channel_pair = 3, .sign = -1 },
        },
        .deadzone = 0.05f,
    };
    return c;
}

int main(void) {
    motors_config_t c = good();
    assert(calibration_valid(&c));

    c = good(); c.wheels[POS_FR].channel_pair = 0;   // duplicate pair 0
    assert(!calibration_valid(&c));

    c = good(); c.wheels[POS_RR].channel_pair = 4;    // out of range
    assert(!calibration_valid(&c));

    c = good(); c.wheels[POS_FL].sign = 0;            // bad sign
    assert(!calibration_valid(&c));

    c = good(); c.deadzone = -0.1f;                   // bad deadzone
    assert(!calibration_valid(&c));

    c = good(); c.deadzone = 1.0f;                    // deadzone must be < 1
    assert(!calibration_valid(&c));

    printf("test_calibration: all passed\n");
    return 0;
}
