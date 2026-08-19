#include "../main/trim.h"
#include <assert.h>
#include <stdio.h>
#include <math.h>

static int feq(float a, float b) { return fabsf(a - b) < 1e-6f; }

int main(void) {
    float l, r;
    // positive trim slows the left side only
    l = 1.0f; r = 1.0f; trim_apply(&l, &r, 0.1f);
    assert(feq(l, 0.9f) && feq(r, 1.0f));
    // negative trim slows the right side only
    l = 1.0f; r = 1.0f; trim_apply(&l, &r, -0.2f);
    assert(feq(l, 1.0f) && feq(r, 0.8f));
    // zero trim: untouched
    l = 0.5f; r = -0.5f; trim_apply(&l, &r, 0.0f);
    assert(feq(l, 0.5f) && feq(r, -0.5f));
    // works symmetrically in reverse (negative speeds)
    l = -1.0f; r = -1.0f; trim_apply(&l, &r, 0.1f);
    assert(feq(l, -0.9f) && feq(r, -1.0f));
    // clamps out-of-range trim
    l = 1.0f; r = 1.0f; trim_apply(&l, &r, 0.9f);
    assert(feq(l, 0.7f) && feq(r, 1.0f));
    l = 1.0f; r = 1.0f; trim_apply(&l, &r, -0.9f);
    assert(feq(l, 1.0f) && feq(r, 0.7f));
    printf("test_trim: all passed\n");
    return 0;
}
