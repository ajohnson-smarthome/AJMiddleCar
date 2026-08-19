#include "../main/wheel.h"
#include <assert.h>
#include <stdio.h>
#include <math.h>

static int feq(float a, float b) { return fabsf(a - b) < 1e-3f; }

int main(void) {
    // JGA25-370: 11 PPR · 1:21 · ×4 → 924
    wheel_params_t a = { .diameter_mm = 65, .ppr = 11, .gear_x100 = 2100, .quad = 4 };
    assert(feq(wheel_cpr(&a), 924.0f));
    // JGB37-520B: 11 PPR · 1:9 · ×4 → 396
    wheel_params_t b = { .diameter_mm = 65, .ppr = 11, .gear_x100 = 900, .quad = 4 };
    assert(feq(wheel_cpr(&b), 396.0f));
    // fractional gear 1:9.6, ×2 → 11 × 9.6 × 2 = 211.2
    wheel_params_t c = { .diameter_mm = 65, .ppr = 11, .gear_x100 = 960, .quad = 2 };
    assert(feq(wheel_cpr(&c), 211.2f));
    printf("test_wheel: all passed\n");
    return 0;
}
