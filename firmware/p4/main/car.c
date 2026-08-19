#include "car.h"
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_err.h"
#include "cJSON.h"
#include "ramp.h"
#include "mixer.h"
#include "motors.h"
#include "calibration.h"
#include "trim.h"
#include "cfg_json.h"

static const char *TAG = "car";

// Default calibration (Phase 2). Replaced by an NVS-stored table in Phase 5.
static motors_config_t g_cfg = {
    .wheels = {
        [POS_FL] = { .channel_pair = 0, .sign = 1 },
        [POS_FR] = { .channel_pair = 1, .sign = 1 },
        [POS_RL] = { .channel_pair = 2, .sign = 1 },
        [POS_RR] = { .channel_pair = 3, .sign = 1 },
    },
    .deadzone = 0.05f,
};

// Serializes g_cfg access and target planning for concurrent callers (console + WS + httpd).
// The PCA9685 itself is written only by the ramp task (ramp.c).
static SemaphoreHandle_t g_lock;
static int8_t g_trim_pct = 0;   // [-30..30], guarded by g_lock like g_cfg

static float clamp_unit(float v) {
    if (v > 1.0f) return 1.0f;
    if (v < -1.0f) return -1.0f;
    return v;
}

void car_drive(float throttle, float yaw) {
    throttle = clamp_unit(throttle);
    yaw = clamp_unit(yaw);
    side_speeds_t s = mixer_mix(throttle, yaw);

    // Take the lock BEFORE reading g_cfg: car_set_calibration writes it from the httpd
    // task, and a torn read of the config could plan duties for invalid channel pairs.
    // Bounded timeout so a stuck holder can't wedge the watchdog task forever.
    if (g_lock && xSemaphoreTake(g_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        ESP_LOGW(TAG, "drive: mutex busy >200ms, skipping write");
        return;
    }
    trim_apply(&s.left, &s.right, (float)g_trim_pct / 100.0f);
    motor_outputs_t out = motors_plan(s.left, s.right, &g_cfg);
    ramp_set_target(out.duty);
    if (g_lock) xSemaphoreGive(g_lock);

    ESP_LOGD(TAG, "drive t=%.2f y=%.2f -> L=%.2f R=%.2f", throttle, yaw, s.left, s.right);
}

void car_stop(void) {
    car_drive(0.0f, 0.0f);
}

void car_set_calibration(const motors_config_t *cfg) {
    // Must own the lock before writing g_cfg (car_drive reads it under the same lock).
    // On timeout, skip the update entirely — never write unlocked or give an un-taken mutex.
    if (g_lock && xSemaphoreTake(g_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        ESP_LOGW(TAG, "set_calibration: mutex busy >200ms, config NOT updated");
        return;
    }
    g_cfg = *cfg;
    if (g_lock) xSemaphoreGive(g_lock);
}

void car_set_trim(int8_t pct) {
    if (pct > 30) pct = 30;
    if (pct < -30) pct = -30;
    if (g_lock && xSemaphoreTake(g_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        ESP_LOGW(TAG, "set_trim: mutex busy, NOT updated");
        return;
    }
    g_trim_pct = pct;
    if (g_lock) xSemaphoreGive(g_lock);
}

int8_t car_get_trim(void) {
    // Read under the same lock that guards writes (consistency with car_set_trim).
    if (g_lock && xSemaphoreTake(g_lock, pdMS_TO_TICKS(200)) != pdTRUE) return 0;
    int8_t val = g_trim_pct;
    if (g_lock) xSemaphoreGive(g_lock);
    return val;
}

// JSON string in NVS under "trim": {"trim_pct":..}
void car_save_trim(void) {
    char buf[32];
    snprintf(buf, sizeof(buf), "{\"trim_pct\":%d}", car_get_trim());
    cfg_json_save("trim", buf);
}

void car_spin_pair(uint8_t pair, bool forward) {
    if (pair > 3) return;
    motor_outputs_t out = { .duty = {0} };
    const uint16_t duty = 1600;  // ~40% for identification
    out.duty[pair * 2]     = forward ? duty : 0;
    out.duty[pair * 2 + 1] = forward ? 0 : duty;
    if (g_lock && xSemaphoreTake(g_lock, pdMS_TO_TICKS(200)) != pdTRUE) return;
    ramp_set_target(out.duty);
    if (g_lock) xSemaphoreGive(g_lock);
}

void car_init(void) {
    g_lock = xSemaphoreCreateMutex();
    // A missing mutex would mean unsynchronized I2C from console + WS tasks; fail visibly.
    ESP_ERROR_CHECK(g_lock ? ESP_OK : ESP_ERR_NO_MEM);

    motors_config_t loaded;
    if (calibration_load(&loaded)) {
        g_cfg = loaded;
    } else {
        ESP_LOGW(TAG, "no NVS calibration — using default mapping");
    }

    char buf[32];
    if (cfg_json_load("trim", buf, sizeof(buf))) {
        cJSON *j = cJSON_Parse(buf);
        int t;
        if (cfg_json_int(j, "trim_pct", &t) && t >= -30 && t <= 30) g_trim_pct = (int8_t)t;
        cJSON_Delete(j);
    }

    car_stop();  // safety stop
}
