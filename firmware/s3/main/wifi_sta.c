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

/* True from the moment wifi_sta_join issues its own esp_wifi_disconnect() until it is
 * consumed by handle_disconnected — or, if the resulting disconnect event never arrives at
 * all (nothing was connected to begin with, the common case for a first join, or any join
 * from WIFI_IDLE/WIFI_FAILED), until wifi_sta_join clears it itself on the way out.
 *
 * _Atomic, not s_lock-guarded, on purpose: an earlier version set/cleared this only inside
 * lock_take()/lock_give() pairs, and a busy lock at the wrong moment could leave it stuck
 * true forever — the next genuine disconnect, possibly hours later, would then be silently
 * swallowed by the branch below. An atomic store can't fail to happen the way a timed-out
 * mutex acquisition can; that is the guarantee this exists to make structural rather than
 * "usually true". */
static _Atomic bool s_join_in_flight;

/* A lock-free mirror of s_sm.state, updated under s_lock alongside every real write to it.
 * wifi_state.h's wifi_sm_t is a pure, host-tested struct and gains no atomics of its own —
 * this exists solely so wifi_sta_state_name has something race-free to fall back to when the
 * lock is busy, in car.c's _Atomic idiom (see s_trim_pct there). */
static _Atomic wifi_state_t s_state_view = WIFI_IDLE;

/* Whether the radio has actually associated with an access point, as opposed to still looking
 * for one. WIFI_JOINING covers both — the pure state machine models the join as one state,
 * correctly, because from its point of view they are one — but they are entirely different
 * things to tell a person: "I cannot find the car" usually means the car is switched off, and
 * "connecting" means it has been found. WIFI_EVENT_STA_CONNECTED is exactly the boundary, and
 * it is knowledge that belongs here rather than in wifi_state.c, which is pure and host-tested
 * and has no business knowing what an association is.
 *
 * _Atomic and not s_lock-guarded, in this file's established idiom (see s_join_in_flight): a
 * store that cannot fail to happen is worth more here than one that is ordered with the state
 * machine, because the only reader turns it into a label. */
static _Atomic bool s_associated;

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
    if (atomic_exchange(&s_join_in_flight, false)) {
        /* This is the disconnect wifi_sta_join issued deliberately, to leave the interface
         * idle before esp_wifi_set_config. Not a failure: consuming it here — rather than
         * stepping WIFI_EV_DISCONNECTED — is what stops it from charging one of the five
         * attempts and racing wifi_sta_join's own upcoming esp_wifi_connect(). Checked
         * before s_lock is even touched, so consuming it never depends on that lock being
         * free — see s_join_in_flight's own comment for why that matters. */
        return;
    }
    if (!lock_take()) {
        /* No further event follows from this dropped one, so the station will not retry on
         * its own — only a POST /net with a CHANGED value restarts it from here. */
        ESP_LOGE(TAG, "state lock busy — the station will not retry");
        return;
    }
    /* Cleared before the step, not after: a disconnect always means the association is gone,
     * whether this ends in a retry or in WIFI_FAILED, and a retry starts by scanning again. */
    atomic_store(&s_associated, false);
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
        } else if (id == WIFI_EVENT_STA_CONNECTED) {
            /* Associated, but with no address yet. The state machine deliberately does not move
             * here — WIFI_EV_GOT_IP is what completes a join — so this only sharpens the label
             * /status reports, from "searching" to "joining". */
            atomic_store(&s_associated, true);
            ESP_LOGI(TAG, "associated; waiting for an address");
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
    /* Under the lock, like every other call to publish_state_locked() — there is no
     * concurrent reader yet at this point, but this site should not be the one exception to
     * its own "call with s_lock held" precondition. */
    if (lock_take()) {
        publish_state_locked();
        lock_give();
    }

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
        /* Logged, not returned. app_main wraps this call in ESP_ERROR_CHECK, and a stored
         * network the radio will not take must not become a boot loop on a device whose only
         * repair path (POST /ota) needs the rest of app_main to finish. The station exists
         * either way, and a POST /net can restart the join. */
        esp_err_t jerr = wifi_sta_join(&cfg);
        if (jerr != ESP_OK) {
            ESP_LOGW(TAG, "the stored network could not be joined at boot: %s",
                     esp_err_to_name(jerr));
        }
    }
    return ESP_OK;
}

esp_err_t wifi_sta_join(const net_cfg_t *cfg)
{
    /* Set unconditionally — no lock needed, and none can make this fail to happen. See
     * s_join_in_flight's own comment for why that is the point. */
    atomic_store(&s_join_in_flight, true);

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
        atomic_store(&s_join_in_flight, false);
        return err;
    }

    if (lock_take()) {
        /* A fresh join starts by scanning, whatever the radio was doing a moment ago. Without
         * this, a re-point while associated to some other network would report "joining" from
         * the first millisecond and never pass through "searching" at all. */
        atomic_store(&s_associated, false);
        wifi_state_step(&s_sm, WIFI_EV_CONFIGURED);
        publish_state_locked();
        lock_give();
    } else {
        ESP_LOGE(TAG, "state lock busy — join requested without recording it");
    }

    /* Cleared unconditionally, independent of whether the lock_take() above succeeded: by
     * now the disconnect above has either already posted its event (the ordinary case, well
     * ahead of this point) or was a no-op because nothing was connected. Either way this
     * join owns nothing further to suppress, and this store cannot fail to run the way that
     * lock acquisition could — bounding s_join_in_flight to at most the duration of this
     * call, with no path left that could leave it stuck. */
    atomic_store(&s_join_in_flight, false);

    esp_err_t cerr = esp_wifi_connect();
    if (cerr != ESP_OK) {
        ESP_LOGW(TAG, "connect failed: %s", esp_err_to_name(cerr));
        return cerr;
    }
    /* The attempt is under way, which is all this can honestly claim: whether it succeeds is
     * decided later, by the event handlers above, and is readable through
     * wifi_sta_state_name(). A lock that was busy at the publish_state_locked() call above is
     * deliberately NOT an error here — the radio was still told to connect, and reporting a
     * 500 for a join that is actually running would be the less honest answer. */
    return ESP_OK;
}

bool wifi_sta_connected(void)
{
    /* The lock-free mirror, not s_sm.state: this is called from the HTTP task on the POST /net
     * path, and a bounded-wait mutex acquisition would be a worse answer than a read that is
     * at most one transition stale. Staleness is harmless in both directions here — a stale
     * "connected" costs a join that net_api would otherwise have skipped, and a stale
     * "not connected" costs one that wifi_sta_join handles idempotently anyway. */
    return atomic_load(&s_state_view) == WIFI_CONNECTED;
}

/* Split WIFI_JOINING into the two things a person needs told apart. Everything else the pure
 * state machine names is already as specific as it can be. */
static const char *refine(const char *name)
{
    if (strcmp(name, DONGLE_STATE_JOINING) == 0 && !atomic_load(&s_associated)) {
        return DONGLE_STATE_SEARCHING;
    }
    return name;
}

const char *wifi_sta_state_name(void)
{
    if (!lock_take()) {
        /* Race-free, unlike a bare read of s_sm.state would be: s_state_view is _Atomic and
         * updated under the lock alongside it. A stale-but-consistent read is safer than
         * blocking the HTTP task /status runs on. */
        ESP_LOGE(TAG, "state lock busy — /status reports the last-known state");
        wifi_sm_t view = { .state = atomic_load(&s_state_view), .attempts = 0 };
        return refine(wifi_state_name(&view));
    }
    const char *name = wifi_state_name(&s_sm);
    lock_give();
    return refine(name);
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
