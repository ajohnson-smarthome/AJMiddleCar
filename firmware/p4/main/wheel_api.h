#ifndef WHEEL_API_H
#define WHEEL_API_H

#include "esp_err.h"

// Register GET/POST /wheel on the shared httpd. Call after http_server_start().
esp_err_t wheel_api_start(void);

#endif // WHEEL_API_H
