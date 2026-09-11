#ifndef MIXER_H
#define MIXER_H

// Normalized side speeds, each in the range [-1.0, 1.0].
typedef struct {
    float left;
    float right;
} side_speeds_t;

// Mix throttle and yaw (each in [-1, 1]) into left/right side speeds.
// Result is normalized preserving the ratio: both values land within [-1, 1].
//   mix(1,0)   -> {1, 1}    straight
//   mix(0,1)   -> {1,-1}    spin in place
//   mix(0.5,0.5)->{1, 0}    arc
side_speeds_t mixer_mix(float throttle, float yaw);

#endif // MIXER_H
