#ifndef TRIM_API_H
#define TRIM_API_H
#include "esp_err.h"
// Register GET/POST /trim on the shared httpd (straight-line trim pct; persisted to NVS).
esp_err_t trim_api_start(void);
#endif // TRIM_API_H
