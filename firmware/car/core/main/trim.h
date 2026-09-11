#ifndef TRIM_H
#define TRIM_H

// Pure straight-line trim: positive trim slows the LEFT side, negative slows the RIGHT.
// Only ever attenuates (never boosts — 100% is the ceiling). trim in [-0.3..0.3].
// Zero ESP-IDF deps — host-tested.
static inline void trim_apply(float *left, float *right, float trim) {
    if (trim > 0.3f) trim = 0.3f;
    if (trim < -0.3f) trim = -0.3f;
    if (trim > 0) *left *= (1.0f - trim);
    else if (trim < 0) *right *= (1.0f + trim);
}

#endif // TRIM_H
