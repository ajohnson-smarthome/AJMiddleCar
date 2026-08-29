#include "esp_err.h"
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

    ESP_ERROR_CHECK(usb_net_start());
    ESP_LOGI(TAG, "dongle up");
}
