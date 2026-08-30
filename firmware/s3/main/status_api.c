#include <stdbool.h>
#include <stdio.h>

#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"

#include "api_guard.h"
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

    /* Read into locals, in this order, rather than passed as two arguments to one snprintf.
     * C does not order argument evaluation, and these two are not one snapshot: state is taken
     * under wifi_sta's lock, rssi is an unlocked esp_wifi_sta_get_ap_info. Evaluated
     * right-to-left, rssi could be sampled while the station was still down and state a moment
     * later once it was up, publishing {"state":"connected","rssi":0} out of two readings that
     * were each correct. Taking state FIRST leaves only the honest version of that pairing: if
     * state says connected, rssi was read afterwards, so a 0 means the link genuinely dropped
     * in between. Not atomicity — there is no lock spanning both — but an ordering that cannot
     * invent a contradiction.
     *
     * `rssi` is a real reading from the dongle's own receiver, not a placeholder: 0 when not
     * connected, whatever esp_wifi_sta_get_ap_info reports otherwise. */
    const char *net_state = wifi_sta_state_name();
    int net_rssi = (int)wifi_sta_rssi();
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
                     net_state, net_rssi);
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
    /* 8080, not 80: port 80 belongs to the car. relay_tcp.c listens there and forwards
     * straight through to the car's own REST surface, so CarHost.port and the car's contract
     * never move — the dongle is the new thing in the system, so the dongle takes the unusual
     * port. */
    cfg.server_port = DONGLE_PORT;
    /* Four are registered: GET /status here, GET and POST /net (net_api.c), POST /ota
     * (ota_api.c). Six is deliberate headroom, so adding an endpoint is not also a config
     * change — and it is the whole story now rather than a placeholder: the relays added no
     * URI handlers at all, being raw sockets on their own ports, and the API guard is an
     * open_fn rather than a handler. Nothing further is pending against this number. */
    cfg.max_uri_handlers = 6;
    /* Lowered from esp_http_server's default of 7: this device's whole lwIP socket table
     * (CONFIG_LWIP_MAX_SOCKETS, sdkconfig.defaults) is shared with relay_udp.c and
     * relay_tcp.c, which is where the full budget arithmetic lives — the comment there is
     * the one to read for why this number is what it is. This server answers an admin API
     * (/status, /net, /ota), not proxied REST traffic, so a couple of concurrent clients is
     * already generous; it does not need the default's share of a table the relays need
     * far more of. */
    cfg.max_open_sockets = 3;
    /* With max_open_sockets this low, LRU purging is not a nicety — it is what keeps the
     * admin API reachable. HTTPD_DEFAULT_CONFIG leaves lru_purge_enable false, and with it
     * false httpd_server does not even put listen_fd in its read set once every session slot
     * is taken (esp_http_server/src/httpd_main.c: `if (hd->config.lru_purge_enable ||
     * httpd_is_sess_available(hd))`). Three stranded keep-alive sessions — a phone unplugged
     * mid-request, three times — would therefore make /status, /net and POST /ota permanently
     * unreachable, with new connections hanging unaccepted rather than being refused, until a
     * power cycle. POST /ota is the only cable-free way to repair a device that lives in a
     * pocket, so "unreachable until a power cycle" is the one outcome worth spending a
     * session for.
     *
     * Checked against the source rather than assumed: httpd_sess_close_lru picks its victim
     * through httpd_sess_enum's HTTPD_TASK_FIND_LOWEST_LRU case (httpd_sess.c), which
     * considers a session only when `session->for_async_req == false` — so a session parked
     * for an async request is never the one purged, and an in-flight OTA upload cannot be
     * evicted by an unrelated connection arriving behind it. */
    cfg.lru_purge_enable = true;
    /* httpd_config_t has no bind-address field in IDF 6.0.2, so this server always listens on
     * INADDR_ANY — USB and, since the station came up, the car's Wi-Fi too. api_guard_open is
     * what stands in front of that: it runs on every accepted connection, before a request
     * byte is parsed, and refuses (closes the socket) any connection that did not land on
     * DONGLE_HOST. Without it, POST /net (a password) and POST /ota (unauthenticated firmware
     * writes) would both be plainly reachable from the car's network. Read api_guard.h for
     * what that check does and does not establish — it is an address check, not an
     * arrival-interface check, and the difference is written down there rather than glossed
     * over here.
     *
     * close_fn is not optional company for open_fn. Installing an open_fn that can fail
     * reaches a double close() inside esp_http_server on every rejection: httpd_sess_new calls
     * httpd_sess_delete (httpd_sess.c), which closes the fd, and control then returns to
     * httpd_accept_conn (httpd_main.c), whose `exit:` closes the same fd number again. The gap
     * between them is not a few instructions — esp_http_server_dispatch_event -> esp_event_post
     * sits in it with CONFIG_HTTPD_SERVER_EVENT_POST_TIMEOUT (2000 ms in this build), blocking
     * the httpd task with the fd already freed whenever the event queue is full, plus an
     * unthrottled ESP_LOGE to a 115200-baud UART. A relay accepting a connection in that window
     * gets handed the freed fd number and has it closed underneath it — and the consequence is
     * not a lost socket: lwip_select returns EBADF the instant any fd in its sets is dead, and
     * a select() loop that treats an error as a log line and falls through has no wait left in
     * the pass. api_guard_close closes that hole; both relays also grew a bounded delay on a
     * select() error, so neither depends on this being right. */
    cfg.open_fn = api_guard_open;
    cfg.close_fn = api_guard_close;

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
