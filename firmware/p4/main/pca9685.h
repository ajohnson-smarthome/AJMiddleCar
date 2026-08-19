#ifndef PCA9685_H
#define PCA9685_H

#include <stdint.h>
#include "esp_err.h"

// Initialize the I2C bus and the PCA9685 device. Call once before anything else.
esp_err_t pca9685_bus_init(int sda_pin, int scl_pin, uint32_t i2c_speed_hz);

// Configure the PCA9685 PWM frequency (sleep->prescale->wake->restart).
esp_err_t pca9685_init(uint16_t pwm_freq_hz);

// Set PWM duty of channel 0..15, duty 0..4095.
esp_err_t pca9685_set_pwm(uint8_t channel, uint16_t duty);

#endif // PCA9685_H
