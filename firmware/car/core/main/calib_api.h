#ifndef CALIB_API_H
#define CALIB_API_H

#include "esp_err.h"

// Register the calibration REST endpoints on the running HTTP server:
//   GET  /calib        -> {"calibrated":true|false}
//   POST /calib/spin   body {"pair":0..3,"dir":1|0} (1=forward, 0=reverse) — pulses one pair ~0.6s
//   POST /calib/save   body {"wheels":[{"pair":0..3,"sign":-1|1} x4]} in FL,FR,RL,RR order
// Both answer {"ok":true} or {"error":"...","field":"..."}, application/json either way.
// Call after http_server_start().
esp_err_t calib_api_start(void);

#endif // CALIB_API_H
