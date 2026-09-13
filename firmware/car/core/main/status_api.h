#ifndef STATUS_API_H
#define STATUS_API_H
#include <stdbool.h>
#include "esp_err.h"
// Register GET /status (a signed JSON identifying this car + light telemetry).
esp_err_t status_api_start(void);
/* main.c calls this when the NVS format-migration path erased the store, BEFORE
 * status_api_start registers the handler — ordering, not a lock, like the radio fields. */
void status_api_note_nvs_wiped(void);
/* Did the bootloader revert the previous OTA? Read once, lazily, cached: rt_link_start
 * runs before status_api_start, and the hello reply carries this flag too. */
bool status_api_rolled_back(void);
#endif // STATUS_API_H
