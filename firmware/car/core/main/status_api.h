#ifndef STATUS_API_H
#define STATUS_API_H
#include "esp_err.h"
// Register GET /status (a signed JSON identifying this car + light telemetry).
esp_err_t status_api_start(void);
/* main.c calls this when the NVS format-migration path erased the store, BEFORE
 * status_api_start registers the handler — ordering, not a lock, like the radio fields. */
void status_api_note_nvs_wiped(void);
#endif // STATUS_API_H
