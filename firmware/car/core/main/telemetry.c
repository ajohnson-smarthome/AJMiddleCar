#include "telemetry.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include <string.h>
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

/* WHOSE signal. The AP admits four stations, and the dongle is one of them — but so can be a
   bench Mac or a second adapter, so "the AP's station list" can hold two radios at once,
   and sta[0] — which is what this reported — is whichever one the driver enumerates first.
   The app plotted whichever station happened to come first, with nothing to say which.

   The reading is the SESSION OWNER's station when there is one: the MAC list from the
   radio, the MAC->IP table from this side's DHCP server, and the owner's address from
   rt_link, joined here. With no owner, or an owner the tables cannot place (a static
   address, a lease the table has forgotten), the weakest station is reported — the
   conservative reading, and unlike sta[0] a deterministic one. */
static esp_netif_t *s_ap_netif;

static int pick_rssi(const wifi_sta_list_t *sta) {
    if (sta->num <= 0) return 0;

    uint32_t owner = rt_link_owner_ip();
    if (owner != 0u && s_ap_netif != NULL) {
        esp_netif_pair_mac_ip_t pairs[ESP_WIFI_MAX_CONN_NUM];
        int n = sta->num < ESP_WIFI_MAX_CONN_NUM ? sta->num : ESP_WIFI_MAX_CONN_NUM;
        for (int i = 0; i < n; i++) {
            memcpy(pairs[i].mac, sta->sta[i].mac, sizeof(pairs[i].mac));
            pairs[i].ip.addr = 0;
        }
        if (esp_netif_dhcps_get_clients_by_mac(s_ap_netif, n, pairs) == ESP_OK) {
            for (int i = 0; i < n; i++) {
                if (pairs[i].ip.addr == owner) return sta->sta[i].rssi;
            }
        }
    }

    int worst = sta->sta[0].rssi;
    for (int i = 1; i < sta->num; i++) {
        if (sta->sta[i].rssi < worst) worst = sta->sta[i].rssi;
    }
    return worst;
}

static void rssi_task(void *arg) {
    (void)arg;
    /* Once. esp_netif_get_handle_from_ifkey is an IPC into the tcpip thread on this stack
       (see the dongle's display.c for the trace), and the AP netif is created before this
       task exists and never destroyed. */
    s_ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (s_ap_netif == NULL) {
        ESP_LOGW(TAG, "no AP netif to place the owner's station — reporting the weakest");
    }
    for (;;) {
        wifi_sta_list_t sta;
        s_rssi = (esp_wifi_ap_get_sta_list(&sta) == ESP_OK) ? pick_rssi(&sta) : 0;
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

/* Bumped on the rt_link task, read by the httpd task through /status. volatile like
   s_frames/s_trips beside it: aligned u32 loads are atomic on this target, but the
   cross-task access is a fact worth declaring, not an ISA accident worth inheriting. */
static volatile uint32_t s_push_seq;

void telemetry_gather(telemetry_t *out, telem_consumer_t who) {
    /* Counts datagrams the car pushed, so it advances for the push and is merely
       reported to a /status poll — a reader of one channel must be able to order that
       channel's frames without a second reader's reads perturbing the count. */
    if (who == TELEM_PUSH) s_push_seq++;

    out->seq        = s_push_seq;
    out->rssi       = s_rssi;
    out->rx_hz      = fps_now(who);
    out->timeouts   = rt_link_wdt_trips();
    out->uptime_s   = (long)(esp_timer_get_time() / 1000000);
    out->free_heap  = (uint32_t)esp_get_free_heap_size();
    out->calibrated = calibration_is_valid();
    out->owner      = link_src_name(link_owner());
    out->bus_ok     = link_bus_ok();

    /* Filled by video_link once it exists (task 6); until then the group says what is
       true of this build: no camera, nothing sent. */
    out->video_state   = VIDEO_STATE_OFF;
    out->video_fps     = 0;
    out->video_kbps    = 0;
    out->video_dropped = 0;
}

int telemetry_json(char *buf, size_t n) {
    telemetry_t t;
    telemetry_gather(&t, TELEM_PUSH);
    return telemetry_datagram(buf, n, &t);
}
