#include "recovery_api.h"
#include <stdio.h>
#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_check.h"
#include "http_server.h"
#include "recovery.h"

static const char *TAG = "recovery_api";

static esp_err_t recover_get(httpd_req_t *req) {
    bool en; uint16_t win;
    recovery_get_config(&en, &win);
    char buf[48];
    int n = snprintf(buf, sizeof(buf), "{\"enabled\":%s,\"window_ms\":%u}",
                     en ? "true" : "false", win);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, n);
}

static esp_err_t recover_post(httpd_req_t *req) {
    char body[64] = {0};
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty");
    // Body is JSON: {"enabled":true|false,"window_ms":..}
    cJSON *j = cJSON_Parse(body);
    cJSON *je = cJSON_GetObjectItemCaseSensitive(j, "enabled");
    cJSON *jw = cJSON_GetObjectItemCaseSensitive(j, "window_ms");
    if (!cJSON_IsBool(je) || !cJSON_IsNumber(jw)) {
        cJSON_Delete(j);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "need {enabled,window_ms}");
    }
    int en = cJSON_IsTrue(je) ? 1 : 0;
    long win = jw->valueint;
    cJSON_Delete(j);
    if (win < RECOVER_WIN_MIN_MS || win > RECOVER_WIN_MAX_MS) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "window_ms 1000..10000");
    }
    recovery_set_config(en == 1, (uint16_t)win);
    recovery_save();
    return httpd_resp_sendstr(req, "ok");
}

esp_err_t recovery_api_start(void) {
    httpd_handle_t server = http_server_get_handle();
    if (server == NULL) { ESP_LOGE(TAG, "http server not started"); return ESP_FAIL; }
    httpd_uri_t g = { .uri = "/recover", .method = HTTP_GET,  .handler = recover_get };
    httpd_uri_t p = { .uri = "/recover", .method = HTTP_POST, .handler = recover_post };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &g), TAG, "reg GET /recover");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &p), TAG, "reg POST /recover");
    return ESP_OK;
}
