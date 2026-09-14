#include "http_server.h"
#include "esp_log.h"
#include "esp_check.h"
#include "identity.h"
#include "esp_app_desc.h"
#include "contract.h"

static const char *TAG = "http";
static httpd_handle_t s_server = NULL;

// There is no web UI: GET / answers "<device> <fw>" so a stray browser (or a script)
// learns what this device is — the same one-line identity the mock serves, because
// protocol.md calls this endpoint an identity and only the mock was honoring that.
static esp_err_t root_get_handler(httpd_req_t *req) {
    char line[64];
    snprintf(line, sizeof(line), "%s %s\n", CAR_DEVICE_ID,
             esp_app_get_description()->version);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, line);
}

httpd_handle_t http_server_get_handle(void) {
    return s_server;
}

esp_err_t http_server_start(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    // 9 routes: /, GET+POST /config, GET+POST /calibration, POST /calibration/spin,
    // GET /status, POST /ota, GET /snapshot — one /config now covers every domain, so
    // this count no longer moves when contract/car-api.json grows a domain. Above the
    // IDF default of 8; without the bump registration aborts with HANDLERS_FULL and the
    // car comes up with no softAP.
    config.max_uri_handlers = 12;
    // The v2 handlers hold more locals than IDF's default 4096-byte task stack ever
    // proved margin for: status_get builds the identity, the three telemetry groups
    // and the 640-byte envelope buffer (~2.4 KB of locals) on top of esp_http_server's
    // own frames and cJSON's recursion in cfg_get/cfg_post. The P4 has RAM to spare, so
    // this buys headroom instead of chasing the exact high-water mark.
    config.stack_size = 8192;
    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &config), TAG, "httpd start");

    httpd_uri_t root = {
        .uri = PATH_ROOT,
        .method = HTTP_GET,
        .handler = root_get_handler,
    };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &root), TAG, "register " PATH_ROOT);

    ESP_LOGI(TAG, "HTTP server started (API only, no web UI)");
    return ESP_OK;
}
