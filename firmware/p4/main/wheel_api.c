#include "wheel_api.h"
#include <stdio.h>
#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_check.h"
#include "http_server.h"
#include "wheel.h"

static const char *TAG = "wheel_api";

static esp_err_t wheel_get_handler(httpd_req_t *req) {
    wheel_params_t w;
    wheel_get(&w);
    char buf[96];
    int n = snprintf(buf, sizeof(buf),
                     "{\"diameter_mm\":%u,\"ppr\":%u,\"gear_x100\":%u,\"quad\":%u}",
                     w.diameter_mm, w.ppr, w.gear_x100, w.quad);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, n);
}

static esp_err_t wheel_post_handler(httpd_req_t *req) {
    char body[96] = {0};
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty");
    // Body is JSON: {"diameter_mm":..,"ppr":..,"gear_x100":..,"quad":..}
    cJSON *j = cJSON_Parse(body);
    cJSON *jd = cJSON_GetObjectItemCaseSensitive(j, "diameter_mm");
    cJSON *jp = cJSON_GetObjectItemCaseSensitive(j, "ppr");
    cJSON *jg = cJSON_GetObjectItemCaseSensitive(j, "gear_x100");
    cJSON *jq = cJSON_GetObjectItemCaseSensitive(j, "quad");
    if (!cJSON_IsNumber(jd) || !cJSON_IsNumber(jp) || !cJSON_IsNumber(jg) || !cJSON_IsNumber(jq)) {
        cJSON_Delete(j);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "need {diameter_mm,ppr,gear_x100,quad}");
    }
    int d = jd->valueint, ppr = jp->valueint, gear = jg->valueint, quad = jq->valueint;
    cJSON_Delete(j);
    if (d < WHEEL_D_MIN_MM || d > WHEEL_D_MAX_MM ||
        ppr < WHEEL_PPR_MIN || ppr > WHEEL_PPR_MAX ||
        gear < WHEEL_GEAR_X100_MIN || gear > WHEEL_GEAR_X100_MAX ||
        (quad != 1 && quad != 2 && quad != 4)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "range: <20..150> <1..1000> <100..30000> <1|2|4>");
    }
    wheel_params_t w = { .diameter_mm = (uint16_t)d, .ppr = (uint16_t)ppr,
                         .gear_x100 = (uint16_t)gear, .quad = (uint8_t)quad };
    wheel_set(&w);
    wheel_save();
    return httpd_resp_sendstr(req, "ok");
}

esp_err_t wheel_api_start(void) {
    httpd_handle_t server = http_server_get_handle();
    if (server == NULL) { ESP_LOGE(TAG, "http server not started"); return ESP_FAIL; }
    httpd_uri_t g = { .uri = "/wheel", .method = HTTP_GET,  .handler = wheel_get_handler };
    httpd_uri_t p = { .uri = "/wheel", .method = HTTP_POST, .handler = wheel_post_handler };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &g), TAG, "reg GET /wheel");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &p), TAG, "reg POST /wheel");
    return ESP_OK;
}
