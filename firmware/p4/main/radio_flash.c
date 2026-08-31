#include "radio_flash.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_hosted.h"
#include "esp_hosted_ota.h"

static const char *TAG = "radio_flash";

/* EMBED_FILES derives these from the file's name; renaming radio_image.bin renames them. */
extern const uint8_t radio_image_start[] asm("_binary_radio_image_bin_start");
extern const uint8_t radio_image_end[]   asm("_binary_radio_image_bin_end");

/* The vendor's cap. Larger writes are rejected by the RPC, not split for us. */
#define RADIO_CHUNK 1536

size_t radio_flash_image_size(void)
{
    return (size_t)(radio_image_end - radio_image_start);
}

const char *radio_flash_version(void)
{
    static char cached[24];
    static bool read;
    if (read) return cached;
    read = true;
    esp_hosted_coprocessor_fwver_t v;
    if (esp_hosted_get_coprocessor_fwversion(&v) != 0) {
        snprintf(cached, sizeof(cached), "unavailable");
        ESP_LOGW(TAG, "could not read radio firmware version");
        return cached;
    }
    snprintf(cached, sizeof(cached), "%u.%u.%u",
             (unsigned)v.major1, (unsigned)v.minor1, (unsigned)v.patch1);
    return cached;
}

void radio_flash_apply(void)
{
    const size_t total = radio_flash_image_size();
    ESP_LOGW(TAG, "flashing the radio: %u bytes over SDIO", (unsigned)total);

    if (esp_hosted_cp_ota_begin() != ESP_OK) {
        /* Nothing was written and no SDIO state changed, so there is nothing to recover from by
         * restarting — and a restart here would cost the owner a whole boot cycle, three times
         * over, before the car serves at all. The attempt is already charged; let this boot
         * carry on and let the next one try again. */
        ESP_LOGE(TAG, "ota begin refused — leaving the radio alone this boot");
        return;
    }

    size_t sent = 0;
    while (sent < total) {
        const size_t n = (total - sent) < RADIO_CHUNK ? (total - sent) : RADIO_CHUNK;
        if (esp_hosted_cp_ota_write(radio_image_start + sent, n) != ESP_OK) {
            /* Safe: an interrupted write leaves the slave's running image alone — the update
             * lands in its inactive slot. */
            ESP_LOGE(TAG, "ota write failed at %u/%u — restarting",
                     (unsigned)sent, (unsigned)total);
            esp_restart();
        }
        sent += n;
    }

    /* From here the two known vendor behaviours are expected, and neither is an error: end()
     * may take the SDIO link down as the radio reboots, and activate() may time out against an
     * old slave that already applied the image itself. Log and restart. */
    if (esp_hosted_cp_ota_end() != ESP_OK) {
        ESP_LOGW(TAG, "ota end reported a failure — expected when the link drops as the radio reboots");
    }
    if (esp_hosted_cp_ota_activate() != ESP_OK) {
        ESP_LOGW(TAG, "ota activate reported a failure — expected against a slave older than 2.6.0");
    }
    ESP_LOGW(TAG, "radio image delivered — restarting; the next boot's version read is the verdict");
    esp_restart();
}
