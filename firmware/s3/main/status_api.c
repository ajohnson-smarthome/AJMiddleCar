#include <stdbool.h>
#include <stdio.h>

#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"

#include "dongle_contract.inc"
#include "net_api.h"
#include "status_api.h"
#include "usb_net.h"
#include "wifi_sta.h"

static const char *TAG = "status_api";

static httpd_handle_t s_server;

/* Did the bootloader revert the previous OTA? The other slot is left ESP_OTA_IMG_ABORTED exactly
 * when an update failed its first boot — the one signal a client has that the image it pushed did
 * not survive. Read once at start: the answer cannot change without a reboot. The car's
 * status_api.c carries the twin of this; the duplication is the price of the two firmwares not
 * referencing each other. */
static bool s_rollback = false;

static void read_rollback_state(void)
{
    const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
    esp_ota_img_states_t st;
    s_rollback = other != NULL &&
                 esp_ota_get_state_partition(other, &st) == ESP_OK &&
                 st == ESP_OTA_IMG_ABORTED;
    if (s_rollback) ESP_LOGW(TAG, "the previous OTA was rolled back by the bootloader");
}

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

    /* `rssi` is a real reading from the dongle's own receiver, not a placeholder: 0 when not
     * connected, whatever esp_wifi_sta_get_ap_info reports otherwise. */
    /* 320, not 256. Worst case with the rollback and net fields: 98 bytes of literal template,
     * + 31 (esp_app_desc_t.version is char[32]) + 31 (idf_ver, likewise) + 5 ("false")
     * + 64 (a 32-byte SSID whose every byte escapes to two) + 9 ("connected") + 4 ("-128")
     * + NUL = 243. The margin is deliberate: adding one field should not also be a buffer
     * calculation. */
    char body[320];
    int n = snprintf(body, sizeof(body),
                     "{\"" DONGLE_KEY_DEVICE "\":\"" DONGLE_DEVICE "\","
                     "\"" DONGLE_KEY_FW "\":\"%s\","
                     "\"" DONGLE_KEY_IDF "\":\"%s\","
                     "\"" DONGLE_KEY_USB "\":\"" DONGLE_USB_STATE_UP "\","
                     "\"" DONGLE_KEY_ROLLBACK "\":%s,"
                     "\"" DONGLE_KEY_NET "\":{"
                     "\"" DONGLE_KEY_NET_SSID "\":\"%s\","
                     "\"" DONGLE_KEY_NET_STATE "\":\"%s\","
                     "\"" DONGLE_KEY_NET_RSSI "\":%d}}",
                     app->version, app->idf_ver, s_rollback ? "true" : "false", ssid_esc,
                     wifi_sta_state_name(), (int)wifi_sta_rssi());
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
    read_rollback_state();

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    /* 8080, not 80: port 80 belongs to the car. Plan 4 forwards it straight through to
     * the car's own REST surface so that CarHost.port and the car's contract never move —
     * the dongle is the new thing in the system, so the dongle takes the unusual port. */
    cfg.server_port = DONGLE_PORT;
    /* /status, GET /net, POST /net, and room for Plan 4's additions — sized so that a
     * new endpoint is not also a config change. */
    cfg.max_uri_handlers = 6;
    /* Lowered from esp_http_server's default of 7: this device's whole lwIP socket table
     * (CONFIG_LWIP_MAX_SOCKETS, sdkconfig.defaults) is shared with relay_udp.c and
     * relay_tcp.c, which is where the full budget arithmetic lives — the comment there is
     * the one to read for why this number is what it is. This server answers an admin API
     * (/status, /net, /ota), not proxied REST traffic, so a couple of concurrent clients is
     * already generous; it does not need the default's share of a table the relays need
     * far more of. */
    cfg.max_open_sockets = 3;

    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &cfg), TAG, "cannot start the server");

    static const httpd_uri_t status_uri = {
        .uri = DONGLE_PATH_STATUS,
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
