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
#include "dongle_clock.h"
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

/* Until when a disconnect event is to be read as wifi_sta_join's OWN doing rather than as a
 * failed join. Zero means "no suppression pending"; otherwise it is a millisecond deadline.
 *
 * A DEADLINE and not a flag, which is the whole of it. As a bool this was set before
 * esp_wifi_disconnect() and cleared unconditionally a few lines later, on the stated
 * assumption that the disconnect's event had "already posted (the ordinary case, well ahead
 * of this point)". Nothing enforced that: the event runs on the event-loop task, and this
 * function runs on httpd with an esp_wifi_set_config and a lock_take between the two. When
 * the event lost that race the flag was already false, so its own deliberate disconnect was
 * charged against the five-attempt budget AND answered with a second esp_wifi_connect()
 * racing the one below.
 *
 * Clearing it only in handle_disconnected would have fixed the race and reintroduced the
 * failure the unconditional clear existed to prevent: a disconnect that never produces an
 * event (nothing was connected — the common case for a first join) leaves the suppression
 * armed, and the next genuine disconnect, possibly hours later, is swallowed. A deadline has
 * neither problem. A late event inside the window is still recognised; an event that never
 * comes costs nothing, because the window closes on its own.
 *
 * One second, because it bounds a handoff between two tasks on the same chip, not a network
 * operation. A genuine disconnect arriving within a second of a join request is this join's
 * own, whatever else it might be.
 *
 * _Atomic, not s_lock-guarded, on purpose: an earlier version set/cleared this only inside
 * lock_take()/lock_give() pairs, and a busy lock at the wrong moment could leave it stuck
 * forever. An atomic store can't fail to happen the way a timed-out mutex acquisition can. */
#define JOIN_QUIET_MS 1000u
static _Atomic uint32_t s_join_quiet_until_ms;

/* A lock-free mirror of s_sm.state, updated under s_lock alongside every real write to it.
 * wifi_state.h's wifi_sm_t is a pure, host-tested struct and gains no atomics of its own —
 * this exists solely so wifi_sta_state_name has something race-free to fall back to when the
 * lock is busy, in car.c's _Atomic idiom (see s_trim_pct there). */
static _Atomic wifi_state_t s_state_view = WIFI_IDLE;

/* Same shape, same reason, for s_sm.attempts: a display polling how much of the join budget is
 * spent deserves the same race-free, at-most-one-transition-stale answer wifi_sta_state_name
 * already gets from s_state_view, not a bounded-wait lock of its own. */
static _Atomic uint8_t s_attempts_view;

/* Whether the radio has actually associated with an access point, as opposed to still looking
 * for one. WIFI_JOINING covers both — the pure state machine models the join as one state,
 * correctly, because from its point of view they are one — but they are entirely different
 * things to tell a person: "I cannot find the car" usually means the car is switched off, and
 * "connecting" means it has been found. WIFI_EVENT_STA_CONNECTED is exactly the boundary, and
 * it is knowledge that belongs here rather than in wifi_state.c, which is pure and host-tested
 * and has no business knowing what an association is.
 *
 * _Atomic and not s_lock-guarded, in this file's established idiom (see s_join_quiet_until_ms): a
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
 * have changed s_sm.state, so the lock-free mirror never lags a real transition. Both mirrors
 * together, always: s_sm.attempts can change on a call that leaves s_sm.state untouched (see
 * WIFI_EV_DISCONNECTED's retry branch in wifi_state.c), so publishing only on a state change
 * would let s_attempts_view drift behind the real count. */
static void publish_state_locked(void)
{
    atomic_store(&s_state_view, s_sm.state);
    atomic_store(&s_attempts_view, s_sm.attempts);
}

static void handle_disconnected(const wifi_event_sta_disconnected_t *ev)
{
    uint32_t quiet_until = atomic_load(&s_join_quiet_until_ms);
    if (ev->reason == WIFI_REASON_ASSOC_LEAVE &&
        quiet_until != 0 && (int32_t)(boot_ms() - quiet_until) < 0) {
        /* This is the disconnect wifi_sta_join issued deliberately, to leave the interface
         * idle before esp_wifi_set_config. Not a failure: consuming it here — rather than
         * stepping WIFI_EV_DISCONNECTED — is what stops it from charging one of the five
         * attempts and racing wifi_sta_join's own upcoming esp_wifi_connect(). Checked
         * before s_lock is even touched, so consuming it never depends on that lock being
         * free — see s_join_quiet_until_ms's own comment for why that matters.
         *
         * The REASON is the discriminator; the window only bounds it in time. ESP-IDF
         * reports WIFI_REASON_ASSOC_LEAVE (8) for a disconnect esp_wifi_disconnect() asked
         * for (station-scenarios.rst, the reason-code table), and every way a join can fail
         * — AUTH_FAIL, ASSOC_FAIL, HANDSHAKE_TIMEOUT, NO_AP_FOUND, the AP's own kicks —
         * carries a different one. The window alone was not enough: when the deliberate
         * disconnect had nothing to tear down and produced no event, the window stayed
         * armed across esp_wifi_connect(), and a join the AP rejected within the second was
         * swallowed here as "ours" — no attempt charged, no retry issued, the station left
         * in JOINING with nothing in flight, on every boot with a stored network. Signed
         * difference, so the window is correct across the millisecond counter's wrap. */
        atomic_store(&s_join_quiet_until_ms, 0u);
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

/* The two failure paths inside wifi_sta_join, which leave the radio idle with no request in
 * flight. Stepped under the lock when it can be had; when it cannot, the lock-free mirror is
 * still moved so that /status and the panel stop claiming a link that is gone — the same
 * "a mirror one transition behind beats a lie" trade publish_state_locked makes. */
static void abort_join_locked_or_not(void)
{
    if (lock_take()) {
        atomic_store(&s_associated, false);
        wifi_state_step(&s_sm, WIFI_EV_ABORTED);
        publish_state_locked();
        lock_give();
    } else {
        ESP_LOGE(TAG, "state lock busy — recording the aborted join in the mirror only");
        atomic_store(&s_associated, false);
        atomic_store(&s_state_view, WIFI_FAILED);
    }
}

esp_err_t wifi_sta_join(const net_cfg_t *cfg)
{
    /* Armed unconditionally — no lock needed, and none can make this fail to happen. See
     * s_join_quiet_until_ms's own comment for why that is the point, and why this is a
     * deadline rather than a flag somebody has to remember to clear. */
    uint32_t until = boot_ms() + JOIN_QUIET_MS;
    atomic_store(&s_join_quiet_until_ms, until != 0u ? until : 1u);   /* 0 means "not armed" */

    /* Disconnect BEFORE reconfiguring, not after: an idle interface is what keeps
     * esp_wifi_set_config from returning ESP_ERR_WIFI_STATE ("still connecting") in the
     * first place, rather than merely tolerating it — and "still connecting" is this
     * device's ordinary condition while a bad join is working through its retry budget.
     * The disconnect's own error is ignored: it legitimately fails when there is nothing to
     * tear down, the common case for a first-ever join. s_join_quiet_until_ms (above) is what
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
        /* The state machine is stepped to FAILED, not left alone. An earlier version left
         * it, reasoning that net.state "still describes what the radio is doing — the
         * previous attempt" — but the disconnect four lines up has already torn that attempt
         * down, and its event will be consumed as ours. Left alone, a station that had been
         * CONNECTED kept saying so with the radio idle, wifi_sta_connected() made the next
         * unchanged re-POST a no-op, and both relays went on aiming at a gateway with nothing
         * behind it. FAILED is what is true: not connected, not trying, until the app
         * configures again. The suppression window is deliberately LEFT armed: the
         * disconnect's event is still coming and is still not a failed join. */
        ESP_LOGE(TAG, "set config failed (%s) — the previous association is already down; "
                      "net.state is failed until a new POST /net", esp_err_to_name(err));
        abort_join_locked_or_not();
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

    /* Nothing to clear here any more. The suppression is a deadline, so it expires whether or
     * not the disconnect above ever produced an event — and leaving it armed across this line
     * is what lets a late event still be recognised as ours. The unconditional clear that used
     * to stand here assumed the event had already been delivered by now, which is exactly the
     * race it created. */

    esp_err_t cerr = esp_wifi_connect();
    if (cerr != ESP_OK) {
        /* CONFIGURED was stepped above, so the machine says "joining" about a request the
         * radio just refused to start — nothing is in flight and nothing will retry it. Same
         * remedy as the set_config path: say failed, which is what it is. */
        ESP_LOGW(TAG, "connect failed: %s — net.state is failed until a new POST /net",
                 esp_err_to_name(cerr));
        abort_join_locked_or_not();
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

bool wifi_sta_ap_info(int8_t *rssi, uint8_t *channel)
{
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        /* Zeroed rather than left alone: the sentinel is the answer to "what is the signal
         * when there is no link", and a caller that reads its own uninitialised locals
         * because it forgot to check the return value should see that answer, not a number
         * off the stack. */
        *rssi = 0;
        *channel = 0;
        return false;
    }
    *rssi = ap.rssi;
    *channel = ap.primary;
    return true;
}

/* The consumed attempts of the current budget, out of WIFI_JOIN_ATTEMPTS.
 *
 * Read from the display and the HTTP task, so it comes from the lock-free mirror for the same
 * reason wifi_sta_connected() does: a value at most one transition stale is a better answer
 * than blocking either caller. */
uint8_t wifi_sta_attempts(void)
{
    return atomic_load(&s_attempts_view);
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
