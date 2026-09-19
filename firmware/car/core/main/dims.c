#include "dims.h"
#include <stdint.h>
#include <stdio.h>
#include "cJSON.h"
#include "cfg_json.h"
#include "cfg_value.h"
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
esp_err_t dims_save(void) {
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"track_mm\":%u,\"wheelbase_mm\":%u}",
             s_params.track_mm, s_params.wheelbase_mm);
    return cfg_json_save("dims", buf);
}

void dims_init(void) {
    char buf[64];
    if (cfg_json_load("dims", buf, sizeof(buf))) {
        cJSON *j = cJSON_Parse(buf);
        /* Field by field, held to the contract's bounds before narrowing — see wheel_init
           for why. The record spells the fields as the contract does. */
        bool has[2]; int32_t v[2], x[2];
        for (int i = 0; i < 2; i++) { int n = 0; has[i] = cfg_json_int(j, CFG_DIMS_FIELDS[i].name, &n); v[i] = n; }
        cfg_values_stored(CFG_DIMS_FIELDS, 2, has, v, x);
        dims_params_t d = { .track_mm = (uint16_t)x[0], .wheelbase_mm = (uint16_t)x[1] };
        dims_set(&d);   // clamps + applies
        cJSON_Delete(j);
    }
    ESP_LOGI(TAG, "dims track=%u mm wheelbase=%u mm", s_params.track_mm, s_params.wheelbase_mm);
}
