#define RAMP_HOST_TEST
#include "../main/ramp.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    // rise is bounded
    assert(ramp_step(0, 4095, 100) == 100);
    assert(ramp_step(100, 4095, 100) == 200);
    // rise lands exactly on target (no overshoot)
    assert(ramp_step(4000, 4095, 100) == 4095);
    assert(ramp_step(4095, 4095, 100) == 4095);
    // fall is instant
    assert(ramp_step(4095, 0, 100) == 0);
    assert(ramp_step(2000, 1999, 1) == 1999);
    // ramp off (max_up = 4095) reaches full in one step
    assert(ramp_step(0, 4095, 4095) == 4095);
    // no uint16 overflow near the top
    assert(ramp_step(4090, 4095, 4095) == 4095);
    printf("test_ramp: all passed\n");
    return 0;
}
