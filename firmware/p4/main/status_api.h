#ifndef STATUS_API_H
#define STATUS_API_H
#include "esp_err.h"
// Register GET /status (a signed JSON identifying this car + light telemetry).
esp_err_t status_api_start(void);
#endif // STATUS_API_H
