#ifndef POWER_MONITOR_H
#define POWER_MONITOR_H

#include <stdint.h>
#include "esp_err.h"

// The pack monitor as the battery task sees it: one chip in the pack's plus lead that
// measures voltage, current and power. One implementation today, ina260.c; an INA226 or
// INA228 would be a second file behind this same header, chosen in board.h — the chip's
// registers never leave its driver (docs/research/2026-09-20-ina260-compat-and-wiring.md § 3).
typedef struct {
    int32_t mv;   // pack voltage, mV
    int32_t ma;   // current, mA — discharge positive; a charger through the same shunt reads negative
    int32_t mw;   // power, mW, as the chip computes it from its own averaged samples
} power_sample_t;

// Detect the chip on the shared bus (i2c_bus.h, after i2c_bus_init) and write its
// conversion setup. ESP_ERR_NOT_FOUND when what answers at BOARD_INA260_ADDR is not the chip
// this driver knows — or nothing does — and the bus's own error otherwise. Called once at
// boot; a failure there is `absent`, not a boot failure, and the reads below keep trying.
esp_err_t power_monitor_init(void);

// One reading of all three. Any transaction that did not complete is a failure, after which
// the next call detects and configures afresh before it reads: a module power-cycled on the
// bench comes back at its power-on defaults, and would otherwise answer at the averaging it
// was never told again.
esp_err_t power_monitor_read(power_sample_t *out);

#endif // POWER_MONITOR_H
