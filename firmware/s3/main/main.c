#include "esp_err.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs_flash.h"

#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_net.h"

static const char *TAG = "dongle";

/* Stage 1: the wire exists and nothing consumes it yet. Task 3 hands these frames
 * to lwIP. Returning ESP_OK without reading the buffer is a deliberate discard, not
 * an unfinished path. */
static esp_err_t on_usb_frame(void *buffer, uint16_t len, void *ctx)
{
    (void)buffer;
    (void)ctx;
    ESP_LOGD(TAG, "rx %u bytes from host", (unsigned)len);
    return ESP_OK;
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    const tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));

    tinyusb_net_config_t net_cfg = {
        .on_recv_callback = on_usb_frame,
    };
    /* The NCM interface needs a MAC. The station MAC is a real, per-device address
     * from the efuse block — borrowing it here costs nothing, because this firmware
     * never brings up a station in Plan 1. Plan 3 must revisit this: a station and
     * a USB interface on the same MAC would be two interfaces claiming one address. */
    ESP_ERROR_CHECK(esp_read_mac(net_cfg.mac_addr, ESP_MAC_WIFI_STA));
    ESP_ERROR_CHECK(tinyusb_net_init(&net_cfg));

    ESP_LOGI(TAG, "NCM up: %02x:%02x:%02x:%02x:%02x:%02x",
             net_cfg.mac_addr[0], net_cfg.mac_addr[1], net_cfg.mac_addr[2],
             net_cfg.mac_addr[3], net_cfg.mac_addr[4], net_cfg.mac_addr[5]);
}
