#include "video_cfg.h"
#include <stdio.h>
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "cfg_json.h"

static const char *TAG = "video_cfg";

static SemaphoreHandle_t s_lock;
static uint16_t s_bitrate = VIDEO_CFG_BITRATE_DEFAULT;
static bool     s_enabled = VIDEO_CFG_ENABLED_DEFAULT;

bool video_cfg_set_bitrate(uint16_t kbps) {
    if (kbps < VIDEO_CFG_BITRATE_MIN) kbps = VIDEO_CFG_BITRATE_MIN;
    if (kbps > VIDEO_CFG_BITRATE_MAX) kbps = VIDEO_CFG_BITRATE_MAX;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) return false;
    s_bitrate = kbps;
    xSemaphoreGive(s_lock);
    return true;
}

uint16_t video_cfg_get_bitrate(void) {
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) return s_bitrate;
    uint16_t v = s_bitrate;
    xSemaphoreGive(s_lock);
    return v;
}

bool video_cfg_set_enabled(bool on) {
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) return false;
    s_enabled = on;
    xSemaphoreGive(s_lock);
    return true;
}

bool video_cfg_get_enabled(void) {
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) return s_enabled;
    bool v = s_enabled;
    xSemaphoreGive(s_lock);
    return v;
}

bool video_cfg_set(uint16_t kbps, bool enabled) {
    if (kbps < VIDEO_CFG_BITRATE_MIN) kbps = VIDEO_CFG_BITRATE_MIN;
    if (kbps > VIDEO_CFG_BITRATE_MAX) kbps = VIDEO_CFG_BITRATE_MAX;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) return false;
    s_bitrate = kbps;
    s_enabled = enabled;
    xSemaphoreGive(s_lock);
    return true;
}

void video_cfg_get(uint16_t *kbps, bool *enabled) {
    bool locked = xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE;
    if (kbps) *kbps = s_bitrate;
    if (enabled) *enabled = s_enabled;
    if (locked) xSemaphoreGive(s_lock);
}

esp_err_t video_cfg_save(void) {
    char buf[56];
    snprintf(buf, sizeof(buf), "{\"bitrate_kbps\":%u,\"enabled\":%s}",
             video_cfg_get_bitrate(), video_cfg_get_enabled() ? "true" : "false");
    return cfg_json_save("video", buf);
}

esp_err_t video_cfg_init(void) {
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    char buf[56];
    if (cfg_json_load("video", buf, sizeof(buf))) {
        cJSON *j = cJSON_Parse(buf);
        int v;
        if (cfg_json_int(j, "bitrate_kbps", &v) && v >= VIDEO_CFG_BITRATE_MIN && v <= VIDEO_CFG_BITRATE_MAX) {
            s_bitrate = (uint16_t)v;
        }
        cJSON *je = cJSON_GetObjectItemCaseSensitive(j, "enabled");
        if (cJSON_IsBool(je)) s_enabled = cJSON_IsTrue(je);   /* absent: the default, on */
        cJSON_Delete(j);
    }
    ESP_LOGI(TAG, "bitrate_kbps = %u, %s (boot)", s_bitrate, s_enabled ? "enabled" : "disabled");
    return ESP_OK;
}
