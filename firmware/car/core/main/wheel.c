#include "wheel.h"
#include <stdint.h>
#include <stdio.h>
#include "cJSON.h"
#include "cfg_json.h"
#include "cfg_value.h"
#include "esp_log.h"

static const char *TAG = "wheel";

/* The record's spelling of each contract field, in CFG_WHEEL_FIELDS' order: `ppr`,
   `gear_x100` and `quad` predate the wire's names, and a record is not rewritten for a
   rename. */
static const char *const STORED[] = { "diameter_mm", "ppr", "gear_x100", "quad" };
_Static_assert(sizeof STORED / sizeof *STORED == sizeof CFG_WHEEL_FIELDS / sizeof *CFG_WHEEL_FIELDS,
               "a field the contract added needs its stored spelling here");

static wheel_params_t s_params = {
    .diameter_mm = 65, .ppr = 11, .gear_x100 = 900, .quad = 4,
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
esp_err_t wheel_save(void) {
    char buf[96];
    snprintf(buf, sizeof(buf), "{\"diameter_mm\":%u,\"ppr\":%u,\"gear_x100\":%u,\"quad\":%u}",
             s_params.diameter_mm, s_params.ppr, s_params.gear_x100, s_params.quad);
    return cfg_json_save("wheel", buf);
}

void wheel_init(void) {
    char buf[96];
    if (cfg_json_load("wheel", buf, sizeof(buf))) {
        cJSON *j = cJSON_Parse(buf);
        /* Field by field, like recovery_init, not all four or nothing: a record older than
           one field keeps the three the user chose (car/config, «Запись старше поля»). Held
           to the contract's bounds before narrowing — (uint16_t)65556 is 20, a diameter
           wheel_set's clamp is happy to accept from a record that never said 20. */
        bool has[4]; int32_t v[4], x[4];
        for (int i = 0; i < 4; i++) { int n = 0; has[i] = cfg_json_int(j, STORED[i], &n); v[i] = n; }
        cfg_values_stored(CFG_WHEEL_FIELDS, 4, has, v, x);
        wheel_params_t w = { .diameter_mm = (uint16_t)x[0], .ppr = (uint16_t)x[1],
                             .gear_x100 = (uint16_t)x[2], .quad = (uint8_t)x[3] };
        wheel_set(&w);   // clamps + applies
        cJSON_Delete(j);
    }
    ESP_LOGI(TAG, "wheel d=%u mm ppr=%u gear=%u/100 quad=%u (cpr %.0f)",
             s_params.diameter_mm, s_params.ppr, s_params.gear_x100, s_params.quad,
             (double)wheel_cpr(&s_params));
}
