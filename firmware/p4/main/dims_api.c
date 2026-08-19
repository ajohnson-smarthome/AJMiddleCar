#include "dims_api.h"
#include <stdio.h>
#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_check.h"
#include "http_server.h"
#include "dims.h"

static const char *TAG = "dims_api";

static esp_err_t dims_get_handler(httpd_req_t *req) {
    dims_params_t d;
    dims_get(&d);
    char buf[64];
    int n = snprintf(buf, sizeof(buf),
                     "{\"track_mm\":%u,\"wheelbase_mm\":%u}", d.track_mm, d.wheelbase_mm);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, n);
}

static esp_err_t dims_post_handler(httpd_req_t *req) {
    char body[64] = {0};
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty");
    // Body is JSON: {"track_mm":..,"wheelbase_mm":..}
    cJSON *j = cJSON_Parse(body);
    cJSON *jt = cJSON_GetObjectItemCaseSensitive(j, "track_mm");
    cJSON *jw = cJSON_GetObjectItemCaseSensitive(j, "wheelbase_mm");
    if (!cJSON_IsNumber(jt) || !cJSON_IsNumber(jw)) {
        cJSON_Delete(j);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "need {track_mm,wheelbase_mm}");
    }
    int track = jt->valueint, base = jw->valueint;
    cJSON_Delete(j);
    if (track < DIMS_TRACK_MIN_MM || track > DIMS_TRACK_MAX_MM ||
        base < DIMS_WHEELBASE_MIN_MM || base > DIMS_WHEELBASE_MAX_MM) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "range: <60..300> <90..360>");
    }
    dims_params_t d = { .track_mm = (uint16_t)track, .wheelbase_mm = (uint16_t)base };
    dims_set(&d);
    dims_save();
    return httpd_resp_sendstr(req, "ok");
}

esp_err_t dims_api_start(void) {
    httpd_handle_t server = http_server_get_handle();
    if (server == NULL) { ESP_LOGE(TAG, "http server not started"); return ESP_FAIL; }
    httpd_uri_t g = { .uri = "/dims", .method = HTTP_GET,  .handler = dims_get_handler };
    httpd_uri_t p = { .uri = "/dims", .method = HTTP_POST, .handler = dims_post_handler };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &g), TAG, "reg GET /dims");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &p), TAG, "reg POST /dims");
    return ESP_OK;
}
