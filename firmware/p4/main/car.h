#ifndef CAR_H
#define CAR_H

#include <stdint.h>
#include <stdbool.h>
#include "motors.h"
#include "link.h"

// Load the calibration and trim from NVS (or defaults) and issue a safety stop.
// Call once after link_init(), before any car_drive() call.
void car_init(void);

// Apply a driving intent: throttle and yaw are clamped to [-1, 1], mixed into side
// speeds, trimmed, and planned to eight per-channel duties, which are then offered
// to the arbiter on behalf of `src`.
//
// Returns false when a higher-priority source holds the actuator. Nothing was applied
// in that case, and the caller must not treat the command as a live frame — the
// control watchdog is fed only on a true return.
//
// Lock-free on the read side: the calibration is immutable and published by pointer
// swap, so any task may call this without blocking and without tearing a read.
bool car_drive(link_src_t src, float throttle, float yaw);

// Safety stop on behalf of `src` — car_drive(src, 0, 0). Subject to the same
// arbitration, so a stop from a low-priority source can be refused.
void car_stop(link_src_t src);

// Replace the active calibration table (e.g. after the user saves a new one).
// Publishes a new immutable copy; callers are serialised by the single httpd task.
void car_set_calibration(const motors_config_t *cfg);

// Straight-line trim: pct in [-30..30]; positive slows the left side. Persisted by the API layer.
void car_set_trim(int8_t pct);
int8_t car_get_trim(void);

// Persist the current trim as a JSON string in NVS (the trim value lives in car.c).
void car_save_trim(void);

// Calibration helper: spin ONE raw PCA9685 channel pair (0..3) at low duty to identify
// which physical wheel it is. Bypasses the calibration table. forward=true drives CH_A.
//
// The grant lapses on its own after LINK_HOLD_CALIB_MS, so the pulse ends whether or
// not the caller is still around. Returns false if something outranks calibration.
bool car_spin_pair(uint8_t pair, bool forward);

#endif // CAR_H
