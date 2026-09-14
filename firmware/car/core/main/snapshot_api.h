#ifndef SNAPSHOT_API_H
#define SNAPSHOT_API_H
#include "esp_err.h"
// GET /snapshot: one JPEG of what the camera sees. A bench tool, not an app feature — it
// stays because "what does the camera see" must be answerable without a phone.
esp_err_t snapshot_api_start(void);
#endif
