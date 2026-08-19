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
#include "motors.h"

static const char *TAG = "calib_api";

// Read the POST body into buf (NUL-terminated). Returns 0 on success, -1 otherwise.
static int read_body(httpd_req_t *req, char *buf, size_t n) {
    if (req->content_len <= 0 || (size_t)req->content_len >= n) return -1;
    int got = httpd_req_recv(req, buf, req->content_len);
    if (got <= 0) return -1;
    buf[got] = '\0';
    return 0;
}

// GET /calib -> {"calibrated":true|false}
static esp_err_t calib_get(httpd_req_t *req) {
    motors_config_t tmp;
    bool cal = calibration_load(&tmp);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, cal ? "{\"calibrated\":true}" : "{\"calibrated\":false}");
}

// POST /calib/spin  body "<pair>,<dir>" (dir 1=forward, 0=reverse). Pulses ~0.6s.
static esp_err_t calib_spin(httpd_req_t *req) {
    char b[32];
    if (read_body(req, b, sizeof(b)) != 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
    }
    // Body is JSON: {"pair":0..3,"dir":0|1}
    cJSON *j = cJSON_Parse(b);
    cJSON *jp = cJSON_GetObjectItemCaseSensitive(j, "pair");
    cJSON *jd = cJSON_GetObjectItemCaseSensitive(j, "dir");
    if (!cJSON_IsNumber(jp) || !cJSON_IsNumber(jd)) {
        cJSON_Delete(j);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "need {pair,dir}");
    }
    int pair = jp->valueint, dir = jd->valueint;
    cJSON_Delete(j);
    if (pair < 0 || pair > 3 || (dir != 0 && dir != 1)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "pair 0..3, dir 0|1");
    }
    ESP_LOGI(TAG, "spin pair %d %s", pair, dir ? "fwd" : "rev");
    car_spin_pair((uint8_t)pair, dir != 0);
    vTaskDelay(pdMS_TO_TICKS(600));
    car_stop();
    return httpd_resp_sendstr(req, "ok");
}

// POST /calib/save  body "<p>:<s>,<p>:<s>,<p>:<s>,<p>:<s>" for FL,FR,RL,RR.
static esp_err_t calib_save(httpd_req_t *req) {
    char b[128];
    if (read_body(req, b, sizeof(b)) != 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
    }
    // Body is JSON: {"wheels":[{"pair":..,"sign":..} × 4]} in FL,FR,RL,RR order.
    cJSON *j = cJSON_Parse(b);
    cJSON *arr = cJSON_GetObjectItemCaseSensitive(j, "wheels");
    if (!cJSON_IsArray(arr) || cJSON_GetArraySize(arr) != 4) {
        cJSON_Delete(j);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "need {wheels:[4x{pair,sign}]}");
    }
    motors_config_t cfg = { .deadzone = 0.05f };
    for (int i = 0; i < 4; i++) {
        cJSON *w = cJSON_GetArrayItem(arr, i);
        cJSON *jp = cJSON_GetObjectItemCaseSensitive(w, "pair");
        cJSON *js = cJSON_GetObjectItemCaseSensitive(w, "sign");
        if (!cJSON_IsNumber(jp) || !cJSON_IsNumber(js)) {
            cJSON_Delete(j);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "wheel needs {pair,sign}");
        }
        cfg.wheels[i].channel_pair = (uint8_t)jp->valueint;
        cfg.wheels[i].sign = (int8_t)js->valueint;
    }
    cJSON_Delete(j);
    esp_err_t e = calibration_save(&cfg);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "save rejected: %s", esp_err_to_name(e));
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid calibration");
    }
    car_set_calibration(&cfg);
    ESP_LOGI(TAG, "calibration saved and applied");
    return httpd_resp_sendstr(req, "ok");
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
