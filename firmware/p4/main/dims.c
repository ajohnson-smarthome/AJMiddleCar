#include "dims.h"
#include <stdio.h>
#include "cJSON.h"
#include "cfg_json.h"
#include "esp_log.h"

static const char *TAG = "dims";

static dims_params_t s_params = { .track_mm = 130, .wheelbase_mm = 210 };

static uint16_t clamp_u16(uint16_t v, uint16_t lo, uint16_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

void dims_set(const dims_params_t *in) {
    if (!in) return;
    s_params.track_mm     = clamp_u16(in->track_mm, DIMS_TRACK_MIN_MM, DIMS_TRACK_MAX_MM);
    s_params.wheelbase_mm = clamp_u16(in->wheelbase_mm, DIMS_WHEELBASE_MIN_MM, DIMS_WHEELBASE_MAX_MM);
}

void dims_get(dims_params_t *out) {
    if (out) *out = s_params;
}

// JSON string in NVS under "dims": {"track_mm":..,"wheelbase_mm":..}
void dims_save(void) {
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"track_mm\":%u,\"wheelbase_mm\":%u}",
             s_params.track_mm, s_params.wheelbase_mm);
    cfg_json_save("dims", buf);
}

void dims_init(void) {
    char buf[64];
    if (cfg_json_load("dims", buf, sizeof(buf))) {
        cJSON *j = cJSON_Parse(buf);
        int track, base;
        if (cfg_json_int(j, "track_mm", &track) && cfg_json_int(j, "wheelbase_mm", &base)) {
            dims_params_t d = { .track_mm = (uint16_t)track, .wheelbase_mm = (uint16_t)base };
            dims_set(&d);   // clamps + applies
        }
        cJSON_Delete(j);
    }
    ESP_LOGI(TAG, "dims track=%u mm wheelbase=%u mm", s_params.track_mm, s_params.wheelbase_mm);
}
