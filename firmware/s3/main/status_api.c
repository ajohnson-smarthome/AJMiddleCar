#include <stdio.h>

#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"

#include "dongle_contract.inc"
#include "net_api.h"
#include "status_api.h"
#include "usb_net.h"

static const char *TAG = "status_api";

static httpd_handle_t s_server;

/* The identity key is `device`, spelled as the car's contract spells it
 * (contract/car-api.json, device_field). The app's "which device am I talking to" check
 * should not need two spellings for one question. */
static esp_err_t status_get(httpd_req_t *req)
{
    const esp_app_desc_t *app = esp_app_get_description();

    net_cfg_t cfg;
    const char *ssid = net_api_current(&cfg) ? cfg.ssid : "";

    /* This body is one flat snprintf with no per-field isolation, so a raw '"' or '\' in
     * the SSID would not just corrupt net.ssid — it would break the parse of the WHOLE
     * document, taking device/fw/idf/usb down with it for every client polling this
     * endpoint. net_cfg_validate lets a quote or backslash through on purpose (a real
     * network can be named with one), so this must escape it rather than trust it. Reuse
     * net_cfg's own escaper — the one net_cfg_render_public/net_cfg_render_stored already
     * use — instead of growing a second one here that could drift from it. */
    char ssid_esc[72]; /* worst case: 32 SSID bytes, every one a quote, doubles to 64, +NUL = 65 */
    if (net_cfg_escape(ssid, ssid_esc, sizeof(ssid_esc)) < 0) {
        /* Only reachable if a future field outgrows ssid_esc — then this is the symptom. */
        ESP_LOGE(TAG, "/status could not escape the ssid into its buffer");
        return ESP_FAIL;
    }

    /* `state` is always "idle" in this firmware, and honestly so: there is no radio yet,
     * so there is nothing that could be joining, connected or failed. The field is here
     * rather than added later because its SHAPE is final — Plan 3 gives it the other
     * values without moving a key or changing a caller. */
    char body[256];
    int n = snprintf(body, sizeof(body),
                     "{\"" DONGLE_KEY_DEVICE "\":\"" DONGLE_DEVICE "\","
                     "\"fw\":\"%s\",\"idf\":\"%s\",\"usb\":\"up\","
                     "\"net\":{\"ssid\":\"%s\",\"state\":\"" DONGLE_STATE_IDLE "\",\"rssi\":0}}",
                     app->version, app->idf_ver, ssid_esc);
    if (n < 0 || (size_t)n >= sizeof(body)) {
        /* Same rule as the car's own /status: truncated JSON parses as something else or
         * nothing, and shipping it under a 200 hides exactly that. Only reachable if a
         * future field outgrows the buffer — then this is the symptom. */
        ESP_LOGE(TAG, "/status does not fit its buffer");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, n);
}

esp_err_t status_api_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    /* 8080, not 80: port 80 belongs to the car. Plan 3 forwards it straight through to
     * the car's own REST surface so that CarHost.port and the car's contract never move —
     * the dongle is the new thing in the system, so the dongle takes the unusual port. */
    cfg.server_port = DONGLE_PORT;
    /* /status, GET /net, POST /net, and room for Plan 3's additions — sized so that a
     * new endpoint is not also a config change. */
    cfg.max_uri_handlers = 6;

    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &cfg), TAG, "cannot start the server");

    static const httpd_uri_t status_uri = {
        .uri = "/status",
        .method = HTTP_GET,
        .handler = status_get,
    };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &status_uri), TAG,
                        "cannot register GET /status");

    ESP_LOGI(TAG, "http://%s:%d" DONGLE_PATH_STATUS, USB_NET_ADDR, DONGLE_PORT);
    return ESP_OK;
}

httpd_handle_t status_api_server(void)
{
    return s_server;
}
