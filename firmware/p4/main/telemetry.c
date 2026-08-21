#include "telemetry.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "calibration.h"
#include "motors.h"
#include "rt_link.h"
#include "link.h"

static const char *TAG = "telemetry";

/* Gathering only. The 5 Hz push itself lives in the rt_link loop, which is the task
   that knows where the owner is and already wakes on that beat. */

/* The AP-side RSSI costs an esp_wifi_ap_get_sta_list, which on this board is an RPC
   across SDIO to the C6 — the same class of call whose timeout used to cost five
   seconds of every boot.
   That is why it is sampled on a task of its own rather than wherever telemetry is
   gathered. Gathering happens on rt_link, the task that applies control frames and is
   the only thing that notices silence: a blocked RPC there means commands are not
   applied and a lost link is not declared, and one long enough to trip the task
   watchdog reboots the board mid-drive. Here the same stall costs one stale reading of
   a display value that changes slowly. Published as a plain int and read without a
   lock: an aligned 32-bit load is atomic, and a reader a second behind reports a
   signal level, not a fact. */
static volatile int s_rssi = 0;

static void rssi_task(void *arg) {
    (void)arg;
    for (;;) {
        wifi_sta_list_t sta;
        s_rssi = (esp_wifi_ap_get_sta_list(&sta) == ESP_OK && sta.num > 0)
               ? sta.sta[0].rssi : 0;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

esp_err_t telemetry_start(void) {
    /* Below everything that drives: this task exists to keep a slow, blocking radio
       call away from the ones that do. Not on the task watchdog either — it is allowed
       to be stuck in an RPC, which is the whole point of it being here. */
    if (xTaskCreate(rssi_task, "rssi", 3072, NULL, 2, NULL) != pdPASS) {
        ESP_LOGW(TAG, "no RSSI sampler — telemetry will report 0");
        return ESP_FAIL;
    }
    return ESP_OK;
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
    out->rssi       = s_rssi;
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
