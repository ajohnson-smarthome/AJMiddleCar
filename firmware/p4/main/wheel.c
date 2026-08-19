#include "wheel.h"
#include <stdio.h>
#include "cJSON.h"
#include "cfg_json.h"
#include "esp_log.h"

static const char *TAG = "wheel";

static wheel_params_t s_params = {
    .diameter_mm = 65, .ppr = 11, .gear_x100 = 2100, .quad = 4,
};

static uint16_t clamp_u16(uint16_t v, uint16_t lo, uint16_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

void wheel_set(const wheel_params_t *in) {
    if (!in) return;
    s_params.diameter_mm = clamp_u16(in->diameter_mm, WHEEL_D_MIN_MM, WHEEL_D_MAX_MM);
    s_params.ppr         = clamp_u16(in->ppr, WHEEL_PPR_MIN, WHEEL_PPR_MAX);
    s_params.gear_x100   = clamp_u16(in->gear_x100, WHEEL_GEAR_X100_MIN, WHEEL_GEAR_X100_MAX);
    s_params.quad        = (in->quad == 1 || in->quad == 2 || in->quad == 4) ? in->quad : 4;
}

void wheel_get(wheel_params_t *out) {
    if (out) *out = s_params;
}

// JSON string in NVS under "wheel": {"diameter_mm":..,"ppr":..,"gear_x100":..,"quad":..}
void wheel_save(void) {
    char buf[96];
    snprintf(buf, sizeof(buf), "{\"diameter_mm\":%u,\"ppr\":%u,\"gear_x100\":%u,\"quad\":%u}",
             s_params.diameter_mm, s_params.ppr, s_params.gear_x100, s_params.quad);
    cfg_json_save("wheel", buf);
}

void wheel_init(void) {
    char buf[96];
    if (cfg_json_load("wheel", buf, sizeof(buf))) {
        cJSON *j = cJSON_Parse(buf);
        int d, ppr, gear, quad;
        if (cfg_json_int(j, "diameter_mm", &d) && cfg_json_int(j, "ppr", &ppr) &&
            cfg_json_int(j, "gear_x100", &gear) && cfg_json_int(j, "quad", &quad)) {
            wheel_params_t w = { .diameter_mm = (uint16_t)d, .ppr = (uint16_t)ppr,
                                 .gear_x100 = (uint16_t)gear, .quad = (uint8_t)quad };
            wheel_set(&w);   // clamps + applies
        }
        cJSON_Delete(j);
    }
    ESP_LOGI(TAG, "wheel d=%u mm ppr=%u gear=%u/100 quad=%u (cpr %.0f)",
             s_params.diameter_mm, s_params.ppr, s_params.gear_x100, s_params.quad,
             (double)wheel_cpr(&s_params));
}
