#ifndef MOTORS_H
#define MOTORS_H

#include <stdint.h>

// Physical wheel positions.
typedef enum {
    POS_FL = 0,  // front-left
    POS_FR,      // front-right
    POS_RL,      // rear-left
    POS_RR,      // rear-right
    POS_COUNT
} wheel_pos_t;

// Per-wheel calibration: which PCA9685 channel pair, and direction sign.
typedef struct {
    uint8_t channel_pair;  // MUST be 0..3 (pair*2 = CH_A, pair*2+1 = CH_B); >=4 indexes out of duty[8]
    int8_t  sign;          // +1 normal, -1 direction reversed
} wheel_calib_t;

// Drive configuration: calibration per position + deadzone.
typedef struct {
    wheel_calib_t wheels[POS_COUNT];
    float deadzone;        // |speed| below this -> motor stopped
} motors_config_t;

// 8 PWM duty values, each 0..4095.
typedef struct {
    uint16_t duty[8];
} motor_outputs_t;

// Plan per-channel PWM from side speeds (each in [-1,1]).
// Left side = {FL, RL}, right side = {FR, RR}. Applies calibration sign.
// Pure function, no I/O. Forward: CH_A=duty, CH_B=0. Reverse: opposite.
motor_outputs_t motors_plan(float left, float right, const motors_config_t *cfg);

#endif // MOTORS_H
