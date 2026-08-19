#include "ramp.h"
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "cfg_json.h"
#include "pca9685.h"

static const char *TAG = "ramp";
#define TICK_MS 20            // 50 Hz
#define RAMP_MS_DEFAULT 300
#define RAMP_MS_MAX 2000

static SemaphoreHandle_t s_lock;          // protects s_target + s_ramp_ms
static uint16_t s_target[8];
static uint16_t s_current[8];
static uint16_t s_ramp_ms = RAMP_MS_DEFAULT;

static uint16_t max_up_per_tick(uint16_t ramp_ms) {
    if (ramp_ms < TICK_MS) return 4095;                  // 0 (off) or < one tick: instant
    uint16_t step = (uint16_t)(4095u * TICK_MS / ramp_ms);
    return step ? step : 1;
}

static void ramp_task(void *arg) {
    (void)arg;
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(TICK_MS));
        uint16_t tgt[8], up;
        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(TICK_MS)) != pdTRUE) continue;
        memcpy(tgt, s_target, sizeof(tgt));
        up = max_up_per_tick(s_ramp_ms);
        xSemaphoreGive(s_lock);

        bool dirty = false;
        for (int ch = 0; ch < 8; ch++) {
            uint16_t next = ramp_step(s_current[ch], tgt[ch], up);
            if (next != s_current[ch]) { s_current[ch] = next; dirty = true; }
        }
        if (!dirty) continue;
        for (uint8_t ch = 0; ch < 8; ch++) {            // sole I2C writer after init
            esp_err_t e = pca9685_set_pwm(ch, s_current[ch]);
            if (e != ESP_OK) ESP_LOGE(TAG, "ch%d write failed: %s", ch, esp_err_to_name(e));
        }
    }
}

void ramp_set_target(const uint16_t duty[8]) {
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        ESP_LOGW(TAG, "set_target: lock busy, frame dropped");
        return;
    }
    memcpy(s_target, duty, sizeof(s_target));
    if (s_lock) xSemaphoreGive(s_lock);
}

void ramp_set_ms(uint16_t ms) {
    if (ms > RAMP_MS_MAX) ms = RAMP_MS_MAX;
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) != pdTRUE) return;
    s_ramp_ms = ms;
    if (s_lock) xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "ramp_ms = %u", ms);
}

uint16_t ramp_get_ms(void) {
    // Read under the same lock that guards writes (consistency with ramp_set_ms).
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) != pdTRUE) return s_ramp_ms;
    uint16_t ms = s_ramp_ms;
    if (s_lock) xSemaphoreGive(s_lock);
    return ms;
}

void ramp_save(void) {
    char buf[32];
    snprintf(buf, sizeof(buf), "{\"ramp_ms\":%u}", ramp_get_ms());
    cfg_json_save("ramp", buf);
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

    return xTaskCreate(ramp_task, "ramp", 3072, NULL, 5, NULL) == pdPASS ? ESP_OK : ESP_FAIL;
}
