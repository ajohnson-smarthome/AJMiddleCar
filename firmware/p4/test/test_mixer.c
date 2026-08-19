#include "mixer.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

static int approx(float a, float b) { return fabsf(a - b) < 1e-4f; }

static void check(float t, float y, float el, float er) {
    side_speeds_t s = mixer_mix(t, y);
    if (!approx(s.left, el) || !approx(s.right, er)) {
        printf("FAIL mix(%.2f,%.2f) = {%.4f,%.4f}, expected {%.4f,%.4f}\n",
               t, y, s.left, s.right, el, er);
        assert(0);
    }
}

int main(void) {
    check(0.0f, 0.0f, 0.0f, 0.0f);   // stop
    check(1.0f, 0.0f, 1.0f, 1.0f);   // straight
    check(-1.0f, 0.0f, -1.0f, -1.0f);// reverse
    check(0.0f, 1.0f, 1.0f, -1.0f);  // spin in place
    check(0.0f, -1.0f, -1.0f, 1.0f); // spin the other way
    check(0.5f, 0.5f, 1.0f, 0.0f);   // arc: left hits clamp floor m=1.0 (no scaling)
    check(1.0f, 1.0f, 1.0f, 0.0f);   // saturation: left=2,right=0 -> /2
    check(-0.1f, 1.0f, 0.818182f, -1.0f);  // right-dominant saturation: |right|>|left|
    printf("test_mixer: all passed\n");
    return 0;
}
