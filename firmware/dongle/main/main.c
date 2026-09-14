#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "nvs_flash.h"

#include "display.h"
#include "dongle_contract.inc"
#include "net_api.h"
#include "ota_api.h"
#include "relay_stats.h"
#include "relay_tcp.h"
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
    /* Before status_api_start(): /status must never be asked before the station exists.
     * The station comes up idle and stays idle until the app sends a network over
     * POST /wifi — nothing about the car's network is kept across a reboot (net_api.c). */
    ESP_ERROR_CHECK(wifi_sta_start());
    /* Both relays' bookkeeping, before either task exists — see relay_stats.h. */
    relay_stats_init(relay_stats_shared());
    static const relay_udp_cfg_t rt_relay    = { .port = DONGLE_RELAY_RT_PORT,    .name = "relay_udp",   .priority = 5, .video = false };
    static const relay_udp_cfg_t video_relay = { .port = DONGLE_RELAY_VIDEO_PORT, .name = "relay_video", .priority = 6, .video = true };
    /* After wifi_sta_start(): the relay tasks wait on wifi_sta_gateway() themselves, polling
     * rather than blocking this function, so they only need the station to exist, not to have
     * joined yet. */
    ESP_ERROR_CHECK(relay_udp_start(&rt_relay));
    ESP_ERROR_CHECK(relay_udp_start(&video_relay));
    /* Same reasoning as relay_udp_start() just above: relay_tcp's task waits on
     * wifi_sta_gateway() itself, so this does not need to wait for a join either. */
    ESP_ERROR_CHECK(relay_tcp_start());
    ESP_ERROR_CHECK(status_api_start());
    ESP_ERROR_CHECK(net_api_register(status_api_server()));
    ESP_ERROR_CHECK(ota_api_register(status_api_server()));

    /* Last of the startup calls, and deliberately still ahead of the rollback waiver below.
       Everything above this line is a rollback trigger — the ESP_ERROR_CHECKs panic-reboot, and
       a panic while the image is still PENDING_VERIFY is what puts the previous one back. A
       display fault must not be able to do that: the panel is the least load-bearing thing on
       the board, and an image that boots and serves /ota is the property worth protecting. So
       this is not ESP_ERROR_CHECKed either — a screen that will not start is logged and lived
       with, never a reason to revert firmware that works. After status_api_start() for a second
       reason: display_start() reads status_api_rolled_back(), and status_api_start() is what
       establishes it. */
    esp_err_t disp_ret = display_start();
    if (disp_ret != ESP_OK) {
        ESP_LOGE(TAG, "display did not start (%s) — the panel stays dark and GET /status's "
                      "packet rates stay 0; everything else is unaffected",
                 esp_err_to_name(disp_ret));
    }

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
