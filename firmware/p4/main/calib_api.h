#ifndef CALIB_API_H
#define CALIB_API_H

#include "esp_err.h"

// Register the calibration REST endpoints on the running HTTP server:
//   GET  /calib        -> {"calibrated":true|false}
//   POST /calib/spin   body "<pair>,<dir>"  (dir: 1=forward, 0=reverse) — pulses one pair ~0.6s
//   POST /calib/save   body "<p>:<s>,<p>:<s>,<p>:<s>,<p>:<s>" for FL,FR,RL,RR
// Call after http_server_start().
esp_err_t calib_api_start(void);

#endif // CALIB_API_H
