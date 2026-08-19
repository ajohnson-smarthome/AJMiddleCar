#ifndef DIMS_API_H
#define DIMS_API_H

#include "esp_err.h"

// Register GET/POST /dims on the shared httpd. Call after http_server_start().
esp_err_t dims_api_start(void);

#endif // DIMS_API_H
