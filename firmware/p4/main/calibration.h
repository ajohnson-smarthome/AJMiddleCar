#ifndef CALIBRATION_H
#define CALIBRATION_H

#include <stdbool.h>
#include <stdint.h>
#ifdef ESP_PLATFORM
#  include "esp_err.h"
#else
   typedef int esp_err_t;
#  define ESP_OK             0
#  define ESP_ERR_INVALID_ARG 0x102
#endif
#include "motors.h"

// Validate a calibration table: the 4 wheels must map to channel_pairs {0,1,2,3}
// each exactly once, every sign must be +1 or -1, and deadzone must be in [0,1).
// Pure (no I/O) — static inline so host tests can use it without NVS.
static inline bool calibration_valid(const motors_config_t *cfg) {
    if (cfg->deadzone < 0.0f || cfg->deadzone >= 1.0f) return false;
    unsigned seen = 0;
    for (int p = 0; p < POS_COUNT; p++) {
        uint8_t pair = cfg->wheels[p].channel_pair;
        int8_t sign = cfg->wheels[p].sign;
        if (pair > 3) return false;
        if (sign != 1 && sign != -1) return false;
        if (seen & (1u << pair)) return false;  // duplicate pair
        seen |= (1u << pair);
    }
    return seen == 0x0F;  // pairs 0..3 all present exactly once
}

// Load the saved calibration from NVS into *out. Returns true only if a VALID
// table was found; otherwise false and *out is untouched (caller keeps default).
bool calibration_load(motors_config_t *out);

// Validate and persist a calibration table to NVS. Returns ESP_ERR_INVALID_ARG
// if invalid, otherwise the NVS commit result.
esp_err_t calibration_save(const motors_config_t *cfg);

#endif // CALIBRATION_H
