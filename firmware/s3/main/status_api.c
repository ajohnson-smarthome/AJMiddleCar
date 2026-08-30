#include <stdio.h>

#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"

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

    char body[192];
    int n = snprintf(body, sizeof(body),
                     "{\"device\":\"ajdongle\",\"fw\":\"%s\",\"idf\":\"%s\",\"usb\":\"up\"}",
                     app->version, app->idf_ver);
    if (n < 0 || n >= (int)sizeof(body)) {
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
    cfg.server_port = 8080;
    /* Plan 2 adds POST /net; leave room so that does not become a config change
     * disguised as a feature. */
    cfg.max_uri_handlers = 4;

    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &cfg), TAG, "cannot start the server");

    static const httpd_uri_t status_uri = {
        .uri = "/status",
        .method = HTTP_GET,
        .handler = status_get,
    };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &status_uri), TAG,
                        "cannot register GET /status");

    ESP_LOGI(TAG, "http://%s:8080/status", USB_NET_ADDR);
    return ESP_OK;
}
