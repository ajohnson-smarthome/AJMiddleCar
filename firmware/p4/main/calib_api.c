#include "calib_api.h"
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_check.h"
#include "http_server.h"
#include "calibration.h"
#include "car.h"
#include "link.h"
#include "motors.h"
#include "api_util.h"

static const char *TAG = "calib_api";

// GET /calib -> {"calibrated":true|false}
static esp_err_t calib_get(httpd_req_t *req) {
    /* The cached flag, not a flash read: a GET of a boolean should not open NVS. */
    bool cal = calibration_is_valid();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, cal ? "{\"calibrated\":true}" : "{\"calibrated\":false}");
}

// POST /calib/spin  body {"pair":0..3,"dir":1|0} (1=forward, 0=reverse). Pulses ~0.6s.
static esp_err_t calib_spin(httpd_req_t *req) {
    char b[32];
    if (api_read_body(req, b, sizeof(b)) < 0) {
        return api_reply_error(req, "400 Bad Request", "", "bad body");
    }
    cJSON *j = cJSON_Parse(b);
    cJSON *jp = cJSON_GetObjectItemCaseSensitive(j, "pair");
    cJSON *jd = cJSON_GetObjectItemCaseSensitive(j, "dir");
    if (!cJSON_IsNumber(jp) || !cJSON_IsNumber(jd) ||
        jp->valuedouble != (double)jp->valueint ||
        jd->valuedouble != (double)jd->valueint) {
        cJSON_Delete(j);
        return api_reply_error(req, "400 Bad Request", "", "need integer {pair,dir}");
    }
    int pair = jp->valueint, dir = jd->valueint;
    cJSON_Delete(j);
    if (pair < 0 || pair > 3) {
        return api_reply_error(req, "400 Bad Request", "pair", "pair 0..3");
    }
    if (dir != 0 && dir != 1) {
        return api_reply_error(req, "400 Bad Request", "dir", "dir 0|1");
    }
    ESP_LOGI(TAG, "spin pair %d %s", pair, dir ? "fwd" : "rev");
    if (!car_spin_pair((uint8_t)pair, dir != 0)) {
        /* 409 is the honest code — the request is fine, the actuator is taken. IDF's
           httpd_err_code_t has no 409, so the status line is set directly. */
        return api_reply_error(req, "409 Conflict", "", "actuator busy");
    }
    /* The grant lapses on its own after LINK_HOLD_CALIB_MS, so the pulse ends whether
       or not this handler is still here. The delay is only so the reply lands after
       the wheel has stopped, which is what the wizard's next step assumes. */
    vTaskDelay(pdMS_TO_TICKS(LINK_HOLD_CALIB_MS));
    link_release_must(LINK_SRC_CALIB);
    return api_reply_ok(req);
}

// POST /calib/save  body "<p>:<s>,<p>:<s>,<p>:<s>,<p>:<s>" for FL,FR,RL,RR.
static esp_err_t calib_save(httpd_req_t *req) {
    char b[128];
    if (api_read_body(req, b, sizeof(b)) < 0) {
        return api_reply_error(req, "400 Bad Request", "", "bad body");
    }
    // Body is JSON: {"wheels":[{"pair":..,"sign":..} × 4]} in FL,FR,RL,RR order.
    cJSON *j = cJSON_Parse(b);
    cJSON *arr = cJSON_GetObjectItemCaseSensitive(j, "wheels");
    if (!cJSON_IsArray(arr) || cJSON_GetArraySize(arr) != 4) {
        cJSON_Delete(j);
        return api_reply_error(req, "400 Bad Request", "wheels", "need {wheels:[4x{pair,sign}]}");
    }
    motors_config_t cfg = { .deadzone = 0.05f };
    for (int i = 0; i < 4; i++) {
        cJSON *w = cJSON_GetArrayItem(arr, i);
        cJSON *jp = cJSON_GetObjectItemCaseSensitive(w, "pair");
        cJSON *js = cJSON_GetObjectItemCaseSensitive(w, "sign");
        if (!cJSON_IsNumber(jp) || !cJSON_IsNumber(js) ||
            jp->valuedouble != (double)jp->valueint ||
            js->valuedouble != (double)js->valueint) {
            cJSON_Delete(j);
            return api_reply_error(req, "400 Bad Request", "pair", "wheel needs integer {pair,sign}");
        }
        cfg.wheels[i].channel_pair = (uint8_t)jp->valueint;
        cfg.wheels[i].sign = (int8_t)js->valueint;
    }
    cJSON_Delete(j);
    esp_err_t e = calibration_save(&cfg);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "save rejected: %s", esp_err_to_name(e));
        return api_reply_error(req, "400 Bad Request", "wheels", "invalid calibration");
    }
    car_set_calibration(&cfg);
    calibration_set_valid(true);
    ESP_LOGI(TAG, "calibration saved and applied");
    return api_reply_ok(req);
}

esp_err_t calib_api_start(void) {
    httpd_handle_t server = http_server_get_handle();
    if (server == NULL) {
        ESP_LOGE(TAG, "http server not started");
        return ESP_FAIL;
    }
    httpd_uri_t get  = { .uri = "/calib",      .method = HTTP_GET,  .handler = calib_get };
    httpd_uri_t spin = { .uri = "/calib/spin", .method = HTTP_POST, .handler = calib_spin };
    httpd_uri_t save = { .uri = "/calib/save", .method = HTTP_POST, .handler = calib_save };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &get),  TAG, "reg /calib");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &spin), TAG, "reg /calib/spin");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &save), TAG, "reg /calib/save");
    ESP_LOGI(TAG, "calibration endpoints registered");
    return ESP_OK;
}
