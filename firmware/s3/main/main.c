#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "nvs_flash.h"

#include "net_api.h"
#include "ota_api.h"
#include "relay_udp.h"
#include "status_api.h"
#include "usb_net.h"
#include "wifi_sta.h"

static const char *TAG = "dongle";

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* esp_netif's DHCP server (and IP_EVENT_NETIF_UP) posts through the default event
     * loop. Without one, esp_event_post fails, and every lease logs an ESP_LOGE on the
     * happy path — exactly when a host is working. status_api's esp_http_server shares
     * this need: esp_http_server.h pulls in esp_event.h because httpd_main.c posts
     * ESP_HTTP_SERVER_EVENT through the same default loop, so one loop here serves
     * both callers. */
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(usb_net_start());
    /* Before the server starts: /status reports the configured SSID, and a request
     * arriving before net_api_load() runs would race the write to s_cfg and report an
     * empty one. net_api_load() has no dependency on the server, so there is no reason
     * for it to run after one exists. */
    net_api_load();
    /* After net_api_load(): a stored network, if any, is joined immediately. Before
     * status_api_start(): /status must never be asked before the station exists. */
    ESP_ERROR_CHECK(wifi_sta_start());
    /* After wifi_sta_start(): the relay task waits on wifi_sta_gateway() itself, polling
     * rather than blocking this function, so it only needs the station to exist, not to have
     * joined yet. */
    ESP_ERROR_CHECK(relay_udp_start());
    ESP_ERROR_CHECK(status_api_start());
    ESP_ERROR_CHECK(net_api_register(status_api_server()));
    ESP_ERROR_CHECK(ota_api_register(status_api_server()));

    /* Rollback is waived here and nowhere earlier. Everything above is a rollback trigger:
       the ESP_ERROR_CHECKs panic-reboot on failure, and a panic while the image is still
       PENDING_VERIFY is what makes the bootloader put the previous one back. By this line the
       USB netif is attached and the server is answering on DONGLE_PORT, which is the whole
       property worth protecting — an image that boots but cannot serve /ota is an image that
       needs a cable to undo, and this device lives in a pocket.

       The guard matters: mark_app_valid on an image that is NOT pending is harmless but noisy,
       and reading the state first keeps a normal cable-flashed boot silent. */
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    if (running != NULL &&
        esp_ota_get_state_partition(running, &ota_state) == ESP_OK &&
        ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "first boot of a new image — cancelling rollback");
        esp_err_t mark_ret = esp_ota_mark_app_valid_cancel_rollback();
        if (mark_ret != ESP_OK) {
            /* The call that was supposed to make this "cancelled" is the one that just
               failed. The image is still PENDING_VERIFY, so the bootloader reverts to the
               previous one on the next boot — this log is the only record of why. */
            ESP_LOGE(TAG, "cancel rollback failed (%s) — image stays PENDING_VERIFY, "
                          "bootloader will revert on next boot", esp_err_to_name(mark_ret));
        }
    }
    ESP_LOGI(TAG, "dongle up");
}
