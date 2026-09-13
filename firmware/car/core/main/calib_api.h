#ifndef CALIB_API_H
#define CALIB_API_H

#include "esp_err.h"

// Register the calibration REST endpoints on the running HTTP server:
//   GET  /calibration        -> {"proto":2,"calibrated":…,"wheels":[{"corner","pair","inverted"} x4]}
//   POST /calibration/spin   body {"pair":0..3,"direction":"forward"|"reverse"} — pulses one pair ~0.6s
//   POST /calibration        body {"wheels":[{"corner","pair","inverted"} x4]}, any order — replies the table
// Call after http_server_start().
esp_err_t calib_api_start(void);

#endif // CALIB_API_H
