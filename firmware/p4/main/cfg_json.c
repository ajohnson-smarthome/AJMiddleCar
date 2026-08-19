#include "cfg_json.h"
#include <string.h>
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "cfg_json";
#define NS "car"

esp_err_t cfg_json_save(const char *key, const char *json) {
    nvs_handle_t h;
    esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    char cur[192];
    size_t len = sizeof(cur);
    if (nvs_get_str(h, key, cur, &len) == ESP_OK && strcmp(cur, json) == 0) {
        nvs_close(h);
        return ESP_OK;   // unchanged → skip the flash commit
    }
    e = nvs_set_str(h, key, json);
    if (e == ESP_OK) e = nvs_commit(h);
    if (e != ESP_OK) ESP_LOGW(TAG, "save %s failed: %s", key, esp_err_to_name(e));
    nvs_close(h);
    return e;
}

bool cfg_json_load(const char *key, char *buf, size_t n) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = n;
    esp_err_t e = nvs_get_str(h, key, buf, &len);
    nvs_close(h);
    return e == ESP_OK;
}

bool cfg_json_int(const cJSON *obj, const char *key, int *out) {
    cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsNumber(it)) return false;
    *out = it->valueint;
    return true;
}
