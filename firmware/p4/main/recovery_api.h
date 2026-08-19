#ifndef RECOVERY_API_H
#define RECOVERY_API_H

#include "esp_err.h"

// Register GET/POST /recover on the shared httpd. Call after http_server_start().
esp_err_t recovery_api_start(void);

#endif // RECOVERY_API_H
