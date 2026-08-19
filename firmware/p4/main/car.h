#ifndef CAR_H
#define CAR_H

#include <stdint.h>
#include <stdbool.h>
#include "motors.h"

// Initialize the mutex + default calibration table and issue a safety stop.
// Call once after pca9685 is initialized, before any car_drive() call.
void car_init(void);

// Apply a driving intent. throttle and yaw are each clamped to [-1, 1], then
// mixed into side speeds, planned to per-channel PWM, and written to the PCA9685.
// Thread-safe: the I2C write is serialized by an internal mutex, so the console
// task and the WebSocket task may both call it. Last write wins.
void car_drive(float throttle, float yaw);

// Convenience safety stop (equivalent to car_drive(0, 0)). Requires car_init() first.
void car_stop(void);

// Replace the active calibration table (e.g. after the user saves a new one).
void car_set_calibration(const motors_config_t *cfg);

// Straight-line trim: pct in [-30..30]; positive slows the left side. Persisted by the API layer.
void car_set_trim(int8_t pct);
int8_t car_get_trim(void);

// Persist the current trim as a JSON string in NVS (the trim value lives in car.c).
void car_save_trim(void);

// Calibration helper: spin ONE raw PCA9685 channel pair (0..3) at low duty to
// identify which physical wheel it is. Bypasses the calibration table.
// forward=true drives CH_A, false drives CH_B. Call car_stop() to halt.
void car_spin_pair(uint8_t pair, bool forward);

#endif // CAR_H
