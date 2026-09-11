#ifndef OTA_API_H
#define OTA_API_H
#include "esp_err.h"
// Register POST /ota — streams an app image into the next OTA slot, validates, reboots.
esp_err_t ota_api_start(void);
#endif // OTA_API_H
