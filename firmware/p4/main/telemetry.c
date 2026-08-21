#include "telemetry.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "calibration.h"
#include "motors.h"
#include "rt_link.h"
#include "link.h"

/* Gathering only. The 5 Hz push itself lives in the rt_link loop, which is the task
   that knows where the owner is and already wakes on that beat. */

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
    uint32_t frames = rt_link_frames();
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
    /* Counts datagrams the car pushed, so it advances for the push and is merely
       reported to a /status poll — a reader of one channel must be able to order that
       channel's frames without a second reader's reads perturbing the count. */
    static uint32_t s_push_seq;
    if (who == TELEM_PUSH) s_push_seq++;

    out->seq        = s_push_seq;
    out->rssi       = ap_client_rssi_cached();
    out->rx_fps     = fps_now(who);
    out->wdt_trips  = rt_link_wdt_trips();
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
