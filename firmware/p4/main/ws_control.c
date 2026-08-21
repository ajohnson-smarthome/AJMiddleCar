#include "ws_control.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_check.h"
#include "http_server.h"
#include "control_proto.h"
#include "car.h"
#include "link.h"
#include "watchdog.h"
#include "recovery.h"

static const char *TAG = "ws";

static volatile uint32_t s_frames = 0;
static volatile int s_client_fd = -1;   // single phone client; last connect wins

uint32_t ws_control_frames(void) { return s_frames; }

static esp_err_t ws_handler(httpd_req_t *req) {
    /* No HTTP_GET branch here on purpose. ESP-IDF 6.0.2 does not call the URI handler
       for a WebSocket handshake at all (esp_http_server/src/httpd_uri.c: "if the request
       is websocket handshake, then do not call the uri->handler"), and on data frames it
       sets req->method to 0. Capturing the client on the handshake — which is what 5.4
       allowed and what this file used to do — left s_client_fd at -1 forever, so the
       telemetry push never sent a single frame on this board. The client is captured
       below instead, on the first frame it sends, which is also the honest expression
       of "last connect wins". */

    // First call with max_len = 0 fills frame.len so we know the payload size.
    httpd_ws_frame_t frame = { .type = HTTPD_WS_TYPE_TEXT };
    esp_err_t ret = httpd_ws_recv_frame(req, &frame, 0);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "recv len failed: %s", esp_err_to_name(ret));
        return ret;
    }
    // The probe overwrites frame.type with the wire opcode. Only act on data
    // frames; PING/PONG/CLOSE are handled by the framework (handle_ws_control_frames
    // defaults to false), CONTINUE and anything unexpected are ignored.
    if (frame.type != HTTPD_WS_TYPE_TEXT && frame.type != HTTPD_WS_TYPE_BINARY) {
        return ESP_OK;
    }
    if (frame.len == 0) return ESP_OK;
    if (frame.len > 31) {
        /* Read it out and throw it away. An unread payload is parsed as the next
           frame's header, which desynchronises the stream for good — the connection
           then produces garbage rather than closing, which is much harder to see. */
        uint8_t sink[64];
        size_t want = frame.len;
        while (want > 0) {
            httpd_ws_frame_t chunk = { .type = frame.type };
            size_t n = want < sizeof(sink) ? want : sizeof(sink);
            chunk.payload = sink;
            chunk.len = n;
            if (httpd_ws_recv_frame(req, &chunk, n) != ESP_OK) break;
            want -= n;
        }
        ESP_LOGW(TAG, "dropped an oversized ws frame (%d bytes)", (int)frame.len);
        return ESP_OK;
    }

    uint8_t buf[32];
    frame.payload = buf;
    ret = httpd_ws_recv_frame(req, &frame, sizeof(buf) - 1);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "recv payload failed: %s", esp_err_to_name(ret));
        return ret;
    }
    buf[frame.len] = '\0';

    float t, y;
    if (control_parse_json((const char *)buf, &t, &y) == 0) {
        s_frames++;
        int fd = httpd_req_to_sockfd(req);
        if (fd != s_client_fd) {
            ESP_LOGI(TAG, "ws client fd=%d (was %d)", fd, s_client_fd);
            s_client_fd = fd;
        }
        /* A parsed frame proves the link is alive, which is the only thing this
           watchdog measures. Actuator health is a separate question, answered
           separately by bus_ok — conflating them made every calibration spin look
           like a dropped link, because a refused frame is not a silent one.
           The breadcrumb IS gated on the grant: a refused command never moved the
           car, so recording it would corrupt the path the retreat retraces. */
        watchdog_feed();
        if (car_drive(LINK_SRC_RT, t, y)) {
            recovery_note_command(t, y);
        }
    } else {
        ESP_LOGW(TAG, "bad ws msg: '%s'", (const char *)buf);
    }
    return ESP_OK;
}

esp_err_t ws_control_start(void) {
    httpd_handle_t server = http_server_get_handle();
    if (server == NULL) {
        ESP_LOGE(TAG, "http server not started");
        return ESP_FAIL;
    }
    httpd_uri_t ws = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = ws_handler,
        .is_websocket = true,
    };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &ws), TAG, "register /ws");
    ESP_LOGI(TAG, "WebSocket endpoint registered at /ws");
    return ESP_OK;
}

esp_err_t ws_control_send(const char *data, size_t len) {
    int fd = s_client_fd;
    if (fd < 0) return ESP_OK;  // no client — nothing to do
    httpd_handle_t server = http_server_get_handle();
    if (server == NULL) return ESP_FAIL;

    /* A socket number is reused the moment it closes, so a stale fd can point at a
       REST connection — and pushing a WebSocket frame into an HTTP response is a
       failure with no symptom. Ask the server what this fd actually is. */
    if (httpd_ws_get_fd_info(server, fd) != HTTPD_WS_CLIENT_WEBSOCKET) {
        s_client_fd = -1;
        return ESP_OK;
    }

    httpd_ws_frame_t frame = {
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)data,
        .len = len,
    };
    /* Deliberately do NOT clear s_client_fd on error. A full send buffer is a moment,
       not a disconnection, and clearing on it killed telemetry for the life of an
       otherwise healthy socket. A real disconnection is caught by the check above. */
    return httpd_ws_send_frame_async(server, fd, &frame);
}
