#include "ramp.h"
#include <stdio.h>
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "cfg_json.h"

static const char *TAG = "ramp";
#define RAMP_MS_DEFAULT 300
#define RAMP_MS_MAX 2000

static SemaphoreHandle_t s_lock;          // protects s_ramp_ms
static uint16_t s_ramp_ms = RAMP_MS_DEFAULT;

bool ramp_set_ms(uint16_t ms) {
    if (ms > RAMP_MS_MAX) ms = RAMP_MS_MAX;
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        /* Unreachable while the lock guards one u16 — but a timed-out set that then
           answered ok:true persisted the OLD value under a success reply. */
        ESP_LOGE(TAG, "ramp lock busy — %u not applied", ms);
        return false;
    }
    s_ramp_ms = ms;
    if (s_lock) xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "ramp_ms = %u", ms);
    return true;
}

uint16_t ramp_get_ms(void) {
    // Read under the same lock that guards writes (consistency with ramp_set_ms).
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) != pdTRUE) return s_ramp_ms;
    uint16_t ms = s_ramp_ms;
    if (s_lock) xSemaphoreGive(s_lock);
    return ms;
}

esp_err_t ramp_save(void) {
    char buf[32];
    snprintf(buf, sizeof(buf), "{\"ramp_ms\":%u}", ramp_get_ms());
    return cfg_json_save("ramp", buf);
}

esp_err_t ramp_init(void) {
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    char buf[32];
    if (cfg_json_load("ramp", buf, sizeof(buf))) {
        cJSON *j = cJSON_Parse(buf);
        int v;
        if (cfg_json_int(j, "ramp_ms", &v) && v >= 0 && v <= RAMP_MS_MAX) {
            s_ramp_ms = (uint16_t)v;
        }
        cJSON_Delete(j);
    }
    ESP_LOGI(TAG, "ramp_ms = %u (boot)", s_ramp_ms);
    return ESP_OK;
}
