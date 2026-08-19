#include "trim_api.h"
#include <stdio.h>
#include <stdlib.h>
#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_check.h"
#include "http_server.h"
#include "car.h"

static const char *TAG = "trim_api";

static esp_err_t trim_get(httpd_req_t *req) {
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "{\"trim_pct\":%d}", (int)car_get_trim());
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, n);
}

static esp_err_t trim_post(httpd_req_t *req) {
    char body[32] = {0};
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty");
    cJSON *j = cJSON_Parse(body);
    cJSON *jv = cJSON_GetObjectItemCaseSensitive(j, "trim_pct");
    if (!cJSON_IsNumber(jv)) { cJSON_Delete(j); return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "need {trim_pct}"); }
    long v = jv->valueint;
    cJSON_Delete(j);
    if (v < -30 || v > 30) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "trim_pct must be -30..30");
    }
    car_set_trim((int8_t)v);
    car_save_trim();
    return httpd_resp_sendstr(req, "ok");
}

esp_err_t trim_api_start(void) {
    httpd_handle_t server = http_server_get_handle();
    if (server == NULL) { ESP_LOGE(TAG, "http server not started"); return ESP_FAIL; }
    httpd_uri_t g = { .uri = "/trim", .method = HTTP_GET, .handler = trim_get };
    httpd_uri_t p = { .uri = "/trim", .method = HTTP_POST, .handler = trim_post };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &g), TAG, "reg GET /trim");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &p), TAG, "reg POST /trim");
    return ESP_OK;
}
