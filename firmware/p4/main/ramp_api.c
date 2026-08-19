#include "ramp_api.h"
#include <stdio.h>
#include <stdlib.h>
#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_check.h"
#include "http_server.h"
#include "ramp.h"

static const char *TAG = "ramp_api";

static esp_err_t ramp_get(httpd_req_t *req) {
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "{\"ramp_ms\":%u}", ramp_get_ms());
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, n);
}

static esp_err_t ramp_post(httpd_req_t *req) {
    char body[32] = {0};
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty");
    cJSON *j = cJSON_Parse(body);
    cJSON *jv = cJSON_GetObjectItemCaseSensitive(j, "ramp_ms");
    if (!cJSON_IsNumber(jv)) { cJSON_Delete(j); return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "need {ramp_ms}"); }
    long v = jv->valueint;
    cJSON_Delete(j);
    if (v < 0 || v > 2000) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ramp_ms must be 0..2000");
    }
    ramp_set_ms((uint16_t)v);
    ramp_save();
    return httpd_resp_sendstr(req, "ok");
}

esp_err_t ramp_api_start(void) {
    httpd_handle_t server = http_server_get_handle();
    if (server == NULL) { ESP_LOGE(TAG, "http server not started"); return ESP_FAIL; }
    httpd_uri_t g = { .uri = "/ramp", .method = HTTP_GET, .handler = ramp_get };
    httpd_uri_t p = { .uri = "/ramp", .method = HTTP_POST, .handler = ramp_post };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &g), TAG, "reg GET /ramp");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &p), TAG, "reg POST /ramp");
    return ESP_OK;
}
