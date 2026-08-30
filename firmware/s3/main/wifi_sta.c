#include "wifi_sta.h"

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

static void handle_disconnected(const wifi_event_sta_disconnected_t *ev)
{
    if (!lock_take()) {
        ESP_LOGE(TAG, "state lock busy — disconnected event dropped");
        return;
    }
    wifi_state_t prev = s_sm.state;
    bool retry = wifi_state_step(&s_sm, WIFI_EV_DISCONNECTED);
    bool entered_failed = (prev != WIFI_FAILED && s_sm.state == WIFI_FAILED);
    lock_give();

    if (entered_failed) {
        /* A car that is off and a password that is wrong look identical in /status — both are
         * just "failed". The reason code is what tells them apart, and the console is where
         * that distinction still has to be made once /status stops making it. */
        ESP_LOGW(TAG, "join failed after the retry budget, reason=%u", (unsigned)ev->reason);
    }
    if (retry) {
        esp_wifi_connect();
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
        esp_wifi_connect();
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
    wifi_config_t wc = {0};

    /* net_cfg_validate bounds ssid to NET_SSID_MAX (32) bytes and password to NET_PASS_MAX
     * (63) bytes, so both strncpy calls below fit inside wifi_config_t.sta's uint8_t[32] and
     * uint8_t[64] fields without truncating a value that validation already accepted. */
    strncpy((char *)wc.sta.ssid, cfg->ssid, sizeof(wc.sta.ssid));
    strncpy((char *)wc.sta.password, cfg->password, sizeof(wc.sta.password));

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set config failed: %s", esp_err_to_name(err));
    }

    if (lock_take()) {
        wifi_state_step(&s_sm, WIFI_EV_CONFIGURED);
        lock_give();
    } else {
        ESP_LOGE(TAG, "state lock busy — join requested without recording it");
    }

    /* esp_wifi_disconnect() legitimately errors when there is nothing to tear down — the
     * common case for a first join — and that error is ignored on purpose. Issuing it anyway,
     * before esp_wifi_connect(), is what makes a re-join to a DIFFERENT network actually
     * switch rather than keep the old association. */
    esp_wifi_disconnect();
    esp_wifi_connect();
}

const char *wifi_sta_state_name(void)
{
    if (!lock_take()) {
        /* A stale read is safer than blocking the HTTP task that /status runs on. */
        ESP_LOGE(TAG, "state lock busy — /status reports the last-known state");
        return wifi_state_name(&s_sm);
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
