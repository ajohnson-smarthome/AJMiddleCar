#include "status_api.h"
#include <stdio.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_app_desc.h"
#include "http_server.h"
#include "telemetry.h"
#include "identity.h"
#include "board.h"
#include <string.h>
#include "esp_hosted.h"

static const char *TAG = "status_api";

// The radio co-processor's firmware version, read once at boot. Reading it per request would put
// SDIO traffic on the app's 1.5 s status poll for a value that cannot change without a reboot.
// It matters because the C6's image is flashed by wire and pinned in board.h: a mismatch has no
// other symptom than the radio misbehaving in ways that look like anything else.
static char s_radio_fw[24] = "unavailable";
static bool s_radio_ok     = false;

static void read_radio_version(void) {
    esp_hosted_coprocessor_fwver_t v;
    if (esp_hosted_get_coprocessor_fwversion(&v) != 0) {
        ESP_LOGW(TAG, "could not read radio firmware version");
        return;
    }
    snprintf(s_radio_fw, sizeof(s_radio_fw), "%u.%u.%u",
             (unsigned)v.major1, (unsigned)v.minor1, (unsigned)v.patch1);
    s_radio_ok = (strcmp(s_radio_fw, BOARD_RADIO_SLAVE_FW) == 0);
    if (s_radio_ok) {
        ESP_LOGI(TAG, "radio firmware %s", s_radio_fw);
    } else {
        ESP_LOGW(TAG, "radio firmware %s, expected %s — reflash the C6 (firmware/c6/README.md)",
                 s_radio_fw, BOARD_RADIO_SLAVE_FW);
    }
}

static esp_err_t status_get(httpd_req_t *req) {
    telemetry_t t;
    telemetry_gather(&t, TELEM_STATUS);
    char fields[224];
    if (telemetry_fields(fields, sizeof(fields), &t) < 0) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "telemetry");
    }
    const char *fw = esp_app_get_description()->version;
    char buf[416];
    int n = snprintf(buf, sizeof(buf),
                     "{\"device\":\"" CAR_DEVICE_ID "\",\"fw\":\"%s\",%s,"
                     "\"radio\":{\"fw\":\"%s\",\"expected\":\"" BOARD_RADIO_SLAVE_FW "\",\"ok\":%s}}",
                     fw, fields, s_radio_fw, s_radio_ok ? "true" : "false");
    if (n < 0 || n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, n);
}

esp_err_t status_api_start(void) {
    httpd_handle_t server = http_server_get_handle();
    if (server == NULL) { ESP_LOGE(TAG, "http server not started"); return ESP_FAIL; }
    read_radio_version();
    httpd_uri_t u = { .uri = "/status", .method = HTTP_GET, .handler = status_get };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &u), TAG, "reg /status");
    ESP_LOGI(TAG, "status endpoint registered");
    return ESP_OK;
}
