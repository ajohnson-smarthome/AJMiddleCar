#include "http_server.h"
#include "esp_log.h"
#include "esp_check.h"

static const char *TAG = "http";
static httpd_handle_t s_server = NULL;

// The car is driven by the native iOS app over the real-time UDP channel; this server
// carries only the cold path (/status, /calib*, /ota and the config domains). There is no
// web UI: GET / answers with a short plain-text identity so a stray browser (or a human
// poking around) understands what this device is.
static esp_err_t root_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "AJMiddleCar: use the iOS app (control on UDP, config over REST).\n");
}

httpd_handle_t http_server_get_handle(void) {
    return s_server;
}

esp_err_t http_server_start(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    // 16 URI handlers: /, /status, /ota, /calib*3, and two per config domain — five
    // domains, registered in a loop from the generated table, so this number now moves
    // when contract/car-api.json does. (/ws is gone with the WebSocket.) Well over the
    // IDF default of 8; without the bump registration aborts with HANDLERS_FULL and the
    // car comes up with no softAP.
    config.max_uri_handlers = 20;
    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &config), TAG, "httpd start");

    httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get_handler,
    };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &root), TAG, "register /");

    ESP_LOGI(TAG, "HTTP server started (API only, no web UI)");
    return ESP_OK;
}
