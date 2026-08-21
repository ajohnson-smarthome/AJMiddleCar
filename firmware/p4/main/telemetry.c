#include "telemetry.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "calibration.h"
#include "motors.h"
#include "ws_control.h"
#include "watchdog.h"
#include "link.h"

static const char *TAG = "telemetry";
#define PUSH_PERIOD_MS 200   // 5 Hz

/* The AP-side RSSI costs an esp_wifi_ap_get_sta_list, which on this board is an RPC
   across SDIO to the C6 — the same class of call whose timeout used to cost five
   seconds of every boot. It is a display value that changes slowly, so it is sampled
   at 1 Hz rather than on every frame. */
static int      s_rssi_cached = 0;
static int64_t  s_rssi_at_us  = 0;

static int ap_client_rssi_cached(void) {
    int64_t now = esp_timer_get_time();
    if (s_rssi_at_us == 0 || now - s_rssi_at_us > 1000000) {
        s_rssi_at_us = now;
        wifi_sta_list_t sta;
        s_rssi_cached = (esp_wifi_ap_get_sta_list(&sta) == ESP_OK && sta.num > 0)
                      ? sta.sta[0].rssi : 0;
    }
    return s_rssi_cached;
}

/* One accumulator per consumer — see telem_consumer_t. */
static int fps_now(telem_consumer_t who) {
    static uint32_t last_frames[TELEM_CONSUMERS];
    static int64_t  last_us[TELEM_CONSUMERS];
    uint32_t frames = ws_control_frames();
    int64_t now = esp_timer_get_time();
    int fps = 0;
    if (last_us[who] != 0) {
        int64_t dt = now - last_us[who];
        if (dt > 0 && dt < 10 * 1000000LL) {
            fps = (int)(((int64_t)(uint32_t)(frames - last_frames[who]) * 1000000LL) / dt);
        }
    }
    last_frames[who] = frames;
    last_us[who] = now;
    return fps;
}

void telemetry_gather(telemetry_t *out, telem_consumer_t who) {
    out->rssi       = ap_client_rssi_cached();
    out->ws_fps     = fps_now(who);
    out->wdt_trips  = watchdog_trips();
    out->uptime_s   = (long)(esp_timer_get_time() / 1000000);
    out->heap       = (uint32_t)esp_get_free_heap_size();
    out->calibrated = calibration_is_valid();
    out->ctl        = link_src_name(link_owner());
    out->bus_ok     = link_bus_ok();
}

int telemetry_json(char *buf, size_t n) {
    telemetry_t t;
    telemetry_gather(&t, TELEM_PUSH);
    char fields[224];
    if (telemetry_fields(fields, sizeof(fields), &t) < 0) return -1;
    int r = snprintf(buf, n, "{%s}", fields);
    return (r < 0 || r >= (int)n) ? -1 : r;
}

static void push_task(void *arg) {
    (void)arg;
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(PUSH_PERIOD_MS));
        char buf[288];
        int n = telemetry_json(buf, sizeof(buf));
        if (n > 0) ws_control_send(buf, (size_t)n);
    }
}

esp_err_t telemetry_start(void) {
    /* A task, not an esp_timer callback. httpd_ws_send_frame_async writes from the
       calling context, so a client with a full receive window blocks it for as long as
       the socket's send timeout — which on the esp_timer task, at priority 22, delays
       every other timer in the system, IDF's own included. */
    if (xTaskCreate(push_task, "telemetry", 3072, NULL, 4, NULL) != pdPASS) return ESP_FAIL;
    ESP_LOGI(TAG, "telemetry push started (5 Hz)");
    return ESP_OK;
}
