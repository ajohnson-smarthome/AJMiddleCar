#ifndef I2C_BUS_H
#define I2C_BUS_H

#include "esp_err.h"
#include "driver/i2c_master.h"

// The one I2C master on the header's SDA/SCL. Created once at boot, and every device on
// the wire — both PCA9685 boards and the camera's SCCB — is added to this handle rather
// than opening a bus of its own: the driver refuses a second master on the same port, and
// the pins are the same copper either way. Speed is a per-device property in i2c_master
// (scl_speed_hz on the device config), so the PWM boards at 400 kHz and the camera at
// BOARD_SCCB_HZ share the handle without sharing a clock.
esp_err_t i2c_bus_init(int sda_pin, int scl_pin);

// NULL before i2c_bus_init succeeded. Callers that add devices must check it: a bus that
// failed to come up is diagnosable (bus_ok false, video off), a NULL dereference is not.
i2c_master_bus_handle_t i2c_bus_handle(void);

// Clock a wedged bus free. The only lever the firmware has when a slave holds SDA low.
esp_err_t i2c_bus_recover(void);

#endif // I2C_BUS_H
