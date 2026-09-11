#ifndef CFG_JSON_H
#define CFG_JSON_H

#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
#include "cJSON.h"

// Shared NVS-JSON config persistence helpers (namespace "car"). Each config domain
// (wheel/dims/ramp/recovery/trim/calibration) stores one JSON string per key.

// Persist JSON string `json` under key `key`. No-op (returns ESP_OK) if the stored
// value is already identical — skips the flash erase/commit (avoids needless wear).
esp_err_t cfg_json_save(const char *key, const char *json);

// Load the JSON string for `key` into buf[n]. Returns true iff the key is present.
bool cfg_json_load(const char *key, char *buf, size_t n);

// Extract an integer member: returns true and writes *out iff `obj` has a numeric
// member named `key` (safe on a NULL obj — returns false).
bool cfg_json_int(const cJSON *obj, const char *key, int *out);

#endif // CFG_JSON_H
