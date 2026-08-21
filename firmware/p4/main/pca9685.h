#ifndef PCA9685_H
#define PCA9685_H

#include <stdint.h>
#include "esp_err.h"

// Initialize the I2C bus and both PCA9685 devices. Call once before anything else.
esp_err_t pca9685_bus_init(int sda_pin, int scl_pin, uint32_t i2c_speed_hz);

// Configure the PWM frequency on both devices (sleep->prescale->wake->restart).
esp_err_t pca9685_init(uint16_t pwm_freq_hz);

// Set PWM duty of a LOGICAL channel 0..7, duty 0..4095.
//
// Logical channels are what the rest of the firmware speaks: motors_plan produces duty[8] and
// the ramp task writes those eight in order. This function is the only place that knows they
// live on two chips — see BOARD_PCA_* in board.h for the split.
esp_err_t pca9685_set_pwm(uint8_t channel, uint16_t duty);

// Drive every channel of every board fully off. Used at boot, because the chip's
// registers survive a P4 reset and the firmware's idea of "stopped" does not.
esp_err_t pca9685_zero_all(void);

#endif // PCA9685_H
