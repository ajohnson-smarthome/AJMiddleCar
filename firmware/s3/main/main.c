#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "usb_net.h"

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
     * happy path — exactly when a host is working. Task 4 needs this loop too. */
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(usb_net_start());
    ESP_LOGI(TAG, "dongle up");
}
