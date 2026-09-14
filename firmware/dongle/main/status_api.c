#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "api_guard.h"
#include "api_util.h"
#include "dongle_contract.inc"
#include "net_api.h"
#include "relay_stats.h"
#include "status_api.h"
#include "status_json.h"
#include "usb_net.h"
#include "wifi_sta.h"
#include "wifi_state.h"

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

bool status_api_rolled_back(void)
{
    return s_rollback;
}

static esp_err_t status_get(httpd_req_t *req)
{
    const esp_app_desc_t *app = esp_app_get_description();
    net_cfg_t cfg;
    bool configured = net_api_current(&cfg);

    /* Read order: attempts BEFORE state, state BEFORE the radio's figures — the reasons
       are the ones the previous version of this function gave at length, and they have
       not changed: a count can only be older than the state framing it, and a 0 RSSI
       after a "connected" is a link that dropped in between, not an impossible pairing. */
    unsigned attempts = (unsigned)wifi_sta_attempts();
    const char *wifi_state = wifi_sta_state_name();
    int8_t ap_rssi;
    uint8_t ap_channel;
    bool connected = wifi_sta_ap_info(&ap_rssi, &ap_channel) && wifi_sta_connected();

    relay_stats_t *relay = relay_stats_shared();
    unsigned errno_age = 0;
    if (relay->last_errno != 0) {
        errno_age = (unsigned)(((uint32_t)(esp_timer_get_time() / 1000) - relay->last_fail_ms) / 1000u);
    }

    status_view_t v = {
        .fw = app->version, .idf = app->idf_ver, .rolled_back = s_rollback,
        .usb_state = usb_net_host_attached() ? DONGLE_USB_STATE_UP : DONGLE_USB_STATE_DOWN,
        .ssid = configured ? cfg.ssid : "", .configured = configured,
        .wifi_state = wifi_state, .connected = connected,
        .rssi_dbm = (int)ap_rssi, .channel = (unsigned)ap_channel,
        .attempts_used = attempts, .attempts_max = (unsigned)WIFI_JOIN_ATTEMPTS,
        .to_car_x10 = (unsigned)relay->to_car_x10, .to_phone_x10 = (unsigned)relay->to_phone_x10,
        .udp_sessions = (unsigned)relay->udp_used, .tcp_connections = (unsigned)relay->tcp_used,
        .video_sessions = (unsigned)relay->video_used,
        .video_kbps_x10 = (unsigned)relay->video_kbps_x10,
        .video_dropped = (unsigned)relay->video_dropped,
        .last_errno = relay->last_errno, .last_error_message = strerror(relay->last_errno),
        .last_error_count = (unsigned)relay->errno_count, .last_error_age_s = errno_age,
        .uptime_s = (long)(esp_timer_get_time() / 1000000),
        .free_heap = (unsigned)esp_get_free_heap_size(),
    };
    char body[720];
    int n = status_json_render(&v, body, sizeof(body));
    if (n < 0) {
        ESP_LOGE(TAG, "/status does not fit its buffer");
        return api_reply_error(req, "500 Internal Server Error", DONGLE_ERR_INTERNAL, "", "status too long");
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
    /* Three are registered: GET /status here, POST /wifi (net_api.c), POST /ota
     * (ota_api.c). Six is deliberate headroom, so adding an endpoint is not also a config
     * change — and it is the whole story now rather than a placeholder: the relays added no
     * URI handlers at all, being raw sockets on their own ports, and the API guard is an
     * open_fn rather than a handler. Nothing further is pending against this number. */
    cfg.max_uri_handlers = 6;
    /* The v2 /status frame carries ~1 KB of locals — the view, the 720-byte body, the
     * renderer's scratch — on top of esp_http_server's own frames, which IDF's default
     * 4096-byte task stack never proved margin for. This server is also the dongle's only
     * OTA path, so headroom here is bought rather than measured. */
    cfg.stack_size = 8192;
    /* Lowered from esp_http_server's default of 7: this device's whole lwIP socket table
     * (CONFIG_LWIP_MAX_SOCKETS, sdkconfig.defaults) is shared with relay_udp.c and
     * relay_tcp.c, which is where the full budget arithmetic lives — the comment there is
     * the one to read for why this number is what it is. This server answers an admin API
     * (/status, /wifi, /ota), not proxied REST traffic, so a couple of concurrent clients is
     * already generous; it does not need the default's share of a table the relays need
     * far more of. */
    cfg.max_open_sockets = 3;
    /* With max_open_sockets this low, LRU purging is not a nicety — it is what keeps the
     * admin API reachable. HTTPD_DEFAULT_CONFIG leaves lru_purge_enable false, and with it
     * false httpd_server does not even put listen_fd in its read set once every session slot
     * is taken (esp_http_server/src/httpd_main.c: `if (hd->config.lru_purge_enable ||
     * httpd_is_sess_available(hd))`). Three stranded keep-alive sessions — a phone unplugged
     * mid-request, three times — would therefore make /status, /wifi and POST /ota permanently
     * unreachable, with new connections hanging unaccepted rather than being refused, until a
     * power cycle. POST /ota is the only cable-free way to repair a device that lives in a
     * pocket, so "unreachable until a power cycle" is the one outcome worth spending a
     * session for.
     *
     * Checked against the source, then checked again after review found the first check
     * incomplete: httpd_sess_close_lru does pick its victim through httpd_sess_enum's
     * HTTPD_TASK_FIND_LOWEST_LRU case (httpd_sess.c), which skips a session with
     * `for_async_req == true` — but that guard does not cover an OTA upload in this
     * firmware. ota_api.c reads its body with the synchronous httpd_req_recv, never the
     * async request API, so for_async_req stays false for the whole upload; a purge sweep
     * would not skip it on that basis. What actually makes eviction impossible:
     * httpd_accept_conn — where a purge happens — and the handler currently blocked in
     * httpd_req_recv both run on the same single httpd task (httpd_main.c:272
     * httpd_server(), :311 where it hands ready sessions to their handlers). That task
     * cannot be back in its own select()/accept loop while it is still inside the handler
     * reading the upload's body, so no accept — and therefore no LRU purge — can land
     * mid-upload. */
    cfg.lru_purge_enable = true;
    /* httpd_config_t has no bind-address field in IDF 6.0.2, so this server always listens on
     * INADDR_ANY — USB and, since the station came up, the car's Wi-Fi too. api_guard_open is
     * what stands in front of that: it runs on every accepted connection, before a request
     * byte is parsed, and refuses (closes the socket) any connection that did not land on
     * DONGLE_HOST. Without it, POST /wifi (a password) and POST /ota (unauthenticated firmware
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
