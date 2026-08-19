#include "status_api.h"
#include <stdio.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_app_desc.h"
#include "http_server.h"
#include "telemetry.h"

static const char *TAG = "status_api";

static esp_err_t status_get(httpd_req_t *req) {
    telemetry_t t;
    telemetry_gather(&t);
    char fields[160];
    if (telemetry_fields(fields, sizeof(fields), &t) < 0) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "telemetry");
    }
    const char *fw = esp_app_get_description()->version;
    char buf[224];
    int n = snprintf(buf, sizeof(buf), "{\"device\":\"esp32-car\",\"fw\":\"%s\",%s}", fw, fields);
    if (n < 0 || n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, n);
}

esp_err_t status_api_start(void) {
    httpd_handle_t server = http_server_get_handle();
    if (server == NULL) { ESP_LOGE(TAG, "http server not started"); return ESP_FAIL; }
    httpd_uri_t u = { .uri = "/status", .method = HTTP_GET, .handler = status_get };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &u), TAG, "reg /status");
    ESP_LOGI(TAG, "status endpoint registered");
    return ESP_OK;
}
