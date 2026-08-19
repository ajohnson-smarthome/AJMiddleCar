#include "telemetry.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "calibration.h"
#include "motors.h"
#include "ws_control.h"
#include "watchdog.h"

static const char *TAG = "telemetry";
#define PUSH_PERIOD_US 200000   // 5 Hz

static int ap_client_rssi(void) {
    wifi_sta_list_t sta;
    if (esp_wifi_ap_get_sta_list(&sta) != ESP_OK || sta.num == 0) return 0;
    return sta.sta[0].rssi;
}

// WS frames/sec between consecutive gather() calls (0 on first call or after a >10s gap).
static int ws_fps_now(void) {
    static uint32_t last_frames = 0;
    static int64_t last_us = 0;
    uint32_t frames = ws_control_frames();
    int64_t now = esp_timer_get_time();
    int fps = 0;
    if (last_us != 0) {
        int64_t dt = now - last_us;
        if (dt > 0 && dt < 10 * 1000000LL) {
            fps = (int)(((int64_t)(uint32_t)(frames - last_frames) * 1000000LL) / dt);
        }
    }
    last_frames = frames;
    last_us = now;
    return fps;
}

void telemetry_gather(telemetry_t *out) {
    motors_config_t tmp;
    out->rssi       = ap_client_rssi();
    out->ws_fps     = ws_fps_now();
    out->wdt_trips  = watchdog_trips();
    out->uptime_s   = (long)(esp_timer_get_time() / 1000000);
    out->heap       = (uint32_t)esp_get_free_heap_size();
    out->calibrated = calibration_load(&tmp);
}

int telemetry_json(char *buf, size_t n) {
    telemetry_t t;
    telemetry_gather(&t);
    char fields[160];
    if (telemetry_fields(fields, sizeof(fields), &t) < 0) return -1;
    int r = snprintf(buf, n, "{%s}", fields);
    return (r < 0 || r >= (int)n) ? -1 : r;
}

static void push_cb(void *arg) {
    (void)arg;
    char buf[200];
    int n = telemetry_json(buf, sizeof(buf));
    if (n > 0) ws_control_send(buf, (size_t)n);
}

esp_err_t telemetry_start(void) {
    const esp_timer_create_args_t args = { .callback = push_cb, .name = "telemetry" };
    esp_timer_handle_t h;
    esp_err_t e = esp_timer_create(&args, &h);
    if (e != ESP_OK) return e;
    e = esp_timer_start_periodic(h, PUSH_PERIOD_US);
    if (e == ESP_OK) ESP_LOGI(TAG, "telemetry push started (5 Hz)");
    return e;
}
