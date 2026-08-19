#ifndef RAMP_API_H
#define RAMP_API_H
#include "esp_err.h"
// Register GET/POST /ramp on the shared httpd (acceleration time, ms; persisted to NVS).
esp_err_t ramp_api_start(void);
#endif // RAMP_API_H
