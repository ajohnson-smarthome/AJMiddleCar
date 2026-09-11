#include "wifi_ap.h"
#include <string.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_mac.h"

static const char *TAG = "wifi_ap";

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data) {
    if (id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *e = (wifi_event_ap_staconnected_t *)data;
        ESP_LOGI(TAG, "station " MACSTR " joined, AID=%d", MAC2STR(e->mac), e->aid);
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *e = (wifi_event_ap_stadisconnected_t *)data;
        ESP_LOGI(TAG, "station " MACSTR " left, AID=%d", MAC2STR(e->mac), e->aid);
    }
}

esp_err_t wifi_ap_start(const char *ssid, const char *password) {
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    if (ap_netif == NULL) {
        ESP_LOGE(TAG, "failed to create default wifi AP netif");
        return ESP_FAIL;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init");

    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL),
        TAG, "event reg");

    wifi_config_t wc = {
        .ap = {
            .channel = 1,
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = { .required = false },
        },
    };
    size_t slen = strlen(ssid);
    // 802.11 SSIDs are up to 32 bytes; ssid_len avoids the NUL-termination requirement.
    if (slen > sizeof(wc.ap.ssid)) slen = sizeof(wc.ap.ssid);
    memcpy(wc.ap.ssid, ssid, slen);
    wc.ap.ssid_len = slen;

    size_t plen = strlen(password);
    strlcpy((char *)wc.ap.password, password, sizeof(wc.ap.password));
    if (plen == 0) {
        wc.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "set mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &wc), TAG, "set config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");

    ESP_LOGI(TAG, "softAP started: SSID='%s' %s, IP 192.168.4.1",
             ssid, plen ? "WPA2" : "OPEN");
    return ESP_OK;
}
