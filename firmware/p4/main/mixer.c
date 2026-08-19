#include "mixer.h"

static float absf(float x) { return x < 0.0f ? -x : x; }

side_speeds_t mixer_mix(float throttle, float yaw) {
    float left = throttle + yaw;
    float right = throttle - yaw;

    float m = absf(left);
    if (absf(right) > m) m = absf(right);
    if (m < 1.0f) m = 1.0f;   // never amplify, only normalize on saturation

    side_speeds_t s;
    s.left = left / m;
    s.right = right / m;
    return s;
}
