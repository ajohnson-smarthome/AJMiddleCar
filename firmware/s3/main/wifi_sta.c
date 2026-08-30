#include "wifi_sta.h"

#include <stdatomic.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "net_api.h"
#include "wifi_state.h"

static const char *TAG = "wifi_sta";

/* s_sm and s_gateway are written from the event-loop task, inside the WIFI_EVENT/IP_EVENT
 * handlers below, and read from the HTTP task through wifi_sta_state_name and
 * wifi_sta_gateway (and, for s_sm, from wifi_sta_join, which the HTTP task also calls). A
 * bounded wait, never an infinite one — car.c's idiom in the car's firmware — so a stuck
 * holder cannot wedge a caller. wifi_sta_start creates the lock and initialises s_sm before
 * registering either handler, so there is no window where a handler could run first. */
#define WIFI_STA_LOCK_MS 200

static SemaphoreHandle_t s_lock;
static wifi_sm_t s_sm;
static uint32_t s_gateway;   /* network byte order; meaningful only when s_has_gateway */
static bool s_has_gateway;

/* True from the moment wifi_sta_join issues its own esp_wifi_disconnect() until either that
 * disconnect's event is consumed by handle_disconnected, or wifi_sta_join itself clears it
 * having moved past the point where that event could still be in flight — see both. Guarded
 * by s_lock like everything else here. */
static bool s_join_in_flight;

/* A lock-free mirror of s_sm.state, updated under s_lock alongside every real write to it.
 * wifi_state.h's wifi_sm_t is a pure, host-tested struct and gains no atomics of its own —
 * this exists solely so wifi_sta_state_name has something race-free to fall back to when the
 * lock is busy, in car.c's _Atomic idiom (see s_trim_pct there). */
static _Atomic wifi_state_t s_state_view = WIFI_IDLE;

static bool lock_take(void)
{
    return s_lock != NULL && xSemaphoreTake(s_lock, pdMS_TO_TICKS(WIFI_STA_LOCK_MS)) == pdTRUE;
}

static void lock_give(void)
{
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
}

/* Call with s_lock held, immediately after any wifi_state_step/wifi_state_init call that may
 * have changed s_sm.state, so the lock-free mirror never lags a real transition. */
static void publish_state_locked(void)
{
    atomic_store(&s_state_view, s_sm.state);
}

static void handle_disconnected(const wifi_event_sta_disconnected_t *ev)
{
    if (!lock_take()) {
        /* No further event follows from this dropped one, so the station will not retry on
         * its own — only a POST /net with a CHANGED value restarts it from here. */
        ESP_LOGE(TAG, "state lock busy — the station will not retry");
        return;
    }
    if (s_join_in_flight) {
        /* This is the disconnect wifi_sta_join issued deliberately, to leave the interface
         * idle before esp_wifi_set_config. Not a failure: consuming it here — rather than
         * stepping WIFI_EV_DISCONNECTED — is what stops it from charging one of the five
         * attempts and racing wifi_sta_join's own upcoming esp_wifi_connect(). */
        s_join_in_flight = false;
        lock_give();
        return;
    }
    wifi_state_t prev = s_sm.state;
    bool retry = wifi_state_step(&s_sm, WIFI_EV_DISCONNECTED);
    bool entered_failed = (prev != WIFI_FAILED && s_sm.state == WIFI_FAILED);
    publish_state_locked();
    lock_give();

    if (entered_failed) {
        /* A car that is off and a password that is wrong look identical in /status — both are
         * just "failed". The reason code is what tells them apart, and the console is where
         * that distinction still has to be made once /status stops making it. */
        ESP_LOGW(TAG, "join failed after the retry budget, reason=%u", (unsigned)ev->reason);
    }
    if (retry) {
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "retry connect failed: %s", esp_err_to_name(err));
        }
    }
}

static void handle_start(void)
{
    if (!lock_take()) {
        ESP_LOGE(TAG, "state lock busy — start event dropped");
        return;
    }
    bool joining = (s_sm.state == WIFI_JOINING);
    lock_give();

    if (joining) {
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "connect failed: %s", esp_err_to_name(err));
        }
    }
}

static void handle_got_ip(const ip_event_got_ip_t *ev)
{
    if (!lock_take()) {
        ESP_LOGE(TAG, "state lock busy — got-ip event dropped");
        return;
    }
    s_gateway = ev->ip_info.gw.addr;
    s_has_gateway = true;
    /* Unconditional, whatever state this arrives in — see wifi_state.h's WIFI_EV_GOT_IP
     * comment. An address is proof the join worked, including a late one landing after the
     * budget ran out. */
    wifi_state_step(&s_sm, WIFI_EV_GOT_IP);
    publish_state_locked();
    lock_give();

    ESP_LOGI(TAG, "joined: ip=" IPSTR " gw=" IPSTR, IP2STR(&ev->ip_info.ip),
             IP2STR(&ev->ip_info.gw));
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_DISCONNECTED) {
            handle_disconnected((const wifi_event_sta_disconnected_t *)data);
        } else if (id == WIFI_EVENT_STA_START) {
            handle_start();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        handle_got_ip((const ip_event_got_ip_t *)data);
    }
}

esp_err_t wifi_sta_start(void)
{
    if (esp_netif_create_default_wifi_sta() == NULL) {
        ESP_LOGE(TAG, "cannot create the station netif");
        return ESP_FAIL;
    }

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_cfg), TAG, "wifi init");

    /* Created — and the state machine initialised — before the handlers are even registered,
     * let alone before esp_wifi_start(): once registered, a handler could in principle run on
     * the very next event-loop tick, and s_sm/s_gateway must already be safe to touch. */
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        ESP_LOGE(TAG, "cannot create the state lock");
        return ESP_FAIL;
    }
    wifi_state_init(&s_sm);
    publish_state_locked();   /* no concurrent reader yet, but keep the mirror honest */

    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
                             WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL),
                         TAG, "wifi event register");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
                             IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL),
                         TAG, "ip event register");

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set mode");
    /* Our own NVS blob (net_cfg.c / net_api.c) is the configuration's home, not esp_wifi's
     * own flash copy — RAM storage keeps the two from drifting or double-writing flash. */
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "set storage");

    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");

    net_cfg_t cfg;
    if (net_api_current(&cfg)) {
        wifi_sta_join(&cfg);
    }
    return ESP_OK;
}

void wifi_sta_join(const net_cfg_t *cfg)
{
    if (lock_take()) {
        s_join_in_flight = true;   /* the very next DISCONNECTED this causes is ours to eat */
        lock_give();
    } else {
        ESP_LOGE(TAG, "state lock busy — join proceeding without disconnect suppression");
    }

    /* Disconnect BEFORE reconfiguring, not after: an idle interface is what keeps
     * esp_wifi_set_config from returning ESP_ERR_WIFI_STATE ("still connecting") in the
     * first place, rather than merely tolerating it — and "still connecting" is this
     * device's ordinary condition while a bad join is working through its retry budget.
     * The disconnect's own error is ignored: it legitimately fails when there is nothing to
     * tear down, the common case for a first-ever join. s_join_in_flight (above) is what
     * keeps the resulting event — real or absent — from being mistaken for a failed join
     * of the network being configured below. */
    esp_wifi_disconnect();

    wifi_config_t wc = {0};

    /* net_cfg_validate bounds ssid to NET_SSID_MAX (32) bytes and password to NET_PASS_MAX
     * (63) bytes, so both strncpy calls below fit inside wifi_config_t.sta's uint8_t[32] and
     * uint8_t[64] fields without truncating a value that validation already accepted. */
    strncpy((char *)wc.sta.ssid, cfg->ssid, sizeof(wc.sta.ssid));
    strncpy((char *)wc.sta.password, cfg->password, sizeof(wc.sta.password));

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (err != ESP_OK) {
        /* Leave the state machine untouched: net.state still describes what the RADIO is
         * doing — the previous attempt, if any — while GET /net already reflects what the
         * dongle was just TOLD to join. The two legitimately disagree until this is retried
         * (a corrected POST /net, which restarts the whole budget). Clear the suppression
         * flag here too, rather than leave it waiting for an event a failed set_config will
         * never cause. */
        ESP_LOGE(TAG, "set config failed (%s) — net.state still reflects the previous "
                      "attempt, not this request; GET /net already shows what was requested",
                 esp_err_to_name(err));
        if (lock_take()) {
            s_join_in_flight = false;
            lock_give();
        }
        return;
    }

    if (lock_take()) {
        wifi_state_step(&s_sm, WIFI_EV_CONFIGURED);
        publish_state_locked();
        /* Cleared here, not left for handle_disconnected: by now the disconnect above has
         * either already posted its event (the ordinary case, well ahead of this point) or
         * was a no-op because nothing was connected. Either way this join owns nothing
         * further to suppress, so bounding s_join_in_flight to at most the duration of this
         * call keeps it from ever wedging a later, genuine disconnect. */
        s_join_in_flight = false;
        lock_give();
    } else {
        ESP_LOGE(TAG, "state lock busy — join requested without recording it");
    }

    esp_err_t cerr = esp_wifi_connect();
    if (cerr != ESP_OK) {
        ESP_LOGW(TAG, "connect failed: %s", esp_err_to_name(cerr));
    }
}

const char *wifi_sta_state_name(void)
{
    if (!lock_take()) {
        /* Race-free, unlike a bare read of s_sm.state would be: s_state_view is _Atomic and
         * updated under the lock alongside it. A stale-but-consistent read is safer than
         * blocking the HTTP task /status runs on. */
        ESP_LOGE(TAG, "state lock busy — /status reports the last-known state");
        wifi_sm_t view = { .state = atomic_load(&s_state_view), .attempts = 0 };
        return wifi_state_name(&view);
    }
    const char *name = wifi_state_name(&s_sm);
    lock_give();
    return name;
}

int8_t wifi_sta_rssi(void)
{
    wifi_ap_record_t info;
    if (esp_wifi_sta_get_ap_info(&info) != ESP_OK) {
        return 0;
    }
    return info.rssi;
}

bool wifi_sta_gateway(uint32_t *out_be)
{
    if (!lock_take()) {
        ESP_LOGE(TAG, "state lock busy — gateway read skipped this pass");
        return false;
    }
    bool have = s_has_gateway;
    if (have) {
        *out_be = s_gateway;
    }
    lock_give();
    return have;
}
