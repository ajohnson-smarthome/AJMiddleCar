#include "ota_api.h"

#include <limits.h>
#include <stdint.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "api_util.h"
#include "dongle_contract.inc"

static const char *TAG = "ota_api";

/* A deliberate twin of firmware/p4/main/ota_api.c, not a shared file — the two firmwares do not
 * reference each other. What is missing here is the car's actuator arbitration: the car seizes
 * the motors for the length of the flash and releases them on every failure path, because a
 * refused upload must not leave a car undriveable. The dongle has nothing that moves, so that
 * layer is absent rather than stubbed out. Every check that guards the flash itself is kept. */
static esp_err_t ota_post(httpd_req_t *req)
{
    if (req->content_len < 4096) {  /* reject obviously-bogus uploads before erasing a slot */
        return api_reply_error(req, "400 Bad Request", "", "image too small");
    }
    if (req->content_len > INT_MAX) {  /* guard the (int) cast below: a huge len wraps negative */
        return api_reply_error(req, "400 Bad Request", "", "image too large");
    }
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (part == NULL) {
        api_reply_error(req, "500 Internal Server Error", "", "no ota partition");
        return ESP_FAIL;
    }
    if ((uint32_t)req->content_len > part->size) {
        return api_reply_error(req, "400 Bad Request", "", "image too large");
    }

    esp_ota_handle_t handle = 0;
    /* The exact length is known from Content-Length: erasing only what the image needs, instead
       of OTA_SIZE_UNKNOWN's full 4 MB, saves seconds of erase and flash wear per update — and a
       too-large image fails above rather than after the erase. */
    esp_err_t berr = esp_ota_begin(part, req->content_len, &handle);
    if (berr != ESP_OK) {
        if (berr == ESP_ERR_OTA_ROLLBACK_INVALID_STATE) {
            /* The running image is still PENDING_VERIFY, so IDF refuses to start another update.
               app_main cancels rollback before it returns, so reaching this needs a request that
               beat the last instruction of boot — which USB enumeration alone makes implausible.
               It gets its own message anyway: "ota begin failed" would send someone hunting the
               flash for a fault that is really a race. */
            ESP_LOGE(TAG, "refusing: this image has not finished verifying its own boot");
            return api_reply_error(req, "409 Conflict", "", "image still pending verify");
        }
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(berr));
        api_reply_error(req, "500 Internal Server Error", "", "ota begin failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "OTA -> %s, %d bytes", part->label, (int)req->content_len);

    char buf[1024];
    int remaining = (int)req->content_len;
    int timeouts = 0;  /* bound stalls: a silent client must not wedge the single httpd task */
    while (remaining > 0) {
        int chunk = remaining < (int)sizeof(buf) ? remaining : (int)sizeof(buf);
        int r = httpd_req_recv(req, buf, chunk);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts <= 6) continue;  /* ~6x5s, then abort */
            /* The one genuinely silent failure otherwise: neither this path nor anything inside
               IDF logs a dropped client or an exhausted timeout budget on its own. */
            ESP_LOGW(TAG, "OTA upload abandoned: %d of %d bytes received (recv returned %d)",
                     (int)req->content_len - remaining, (int)req->content_len, r);
            esp_ota_abort(handle);
            api_reply_error(req, "400 Bad Request", "", "recv error");
            return ESP_FAIL;
        }
        timeouts = 0;  /* progress resets the stall budget */
        /* esp_ota_write rejects a first byte that is not 0xE9 (ESP_IMAGE_HEADER_MAGIC) with
           ESP_ERR_OTA_VALIDATE_FAILED — the spec's "reject anything whose first byte is not the
           ESP image magic" is satisfied by IDF, not by a check of ours. Do not add a second one:
           a hand-rolled magic test would be a copy of app_update's that could drift from it. */
        esp_err_t werr = esp_ota_write(handle, buf, r);
        if (werr != ESP_OK) {
            esp_ota_abort(handle);
            if (werr == ESP_ERR_OTA_VALIDATE_FAILED) {
                /* The client sent something that isn't a valid app image — its fault. */
                api_reply_error(req, "400 Bad Request", "", "image invalid");
            } else {
                /* Anything else (e.g. an esp_partition_write flash error) is this device's
                   fault, not the client's. The car's twin (firmware/p4/main/ota_api.c) reports
                   500 for both cases, which is wrong in the other direction — most of its
                   failures here are this same magic-byte rejection, not a device fault. */
                ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(werr));
                api_reply_error(req, "500 Internal Server Error", "", "ota write failed");
            }
            return ESP_FAIL;
        }
        remaining -= r;
    }

    if (esp_ota_end(handle) != ESP_OK) {
        api_reply_error(req, "400 Bad Request", "", "image invalid");
        return ESP_FAIL;
    }
    esp_err_t serr = esp_ota_set_boot_partition(part);
    if (serr != ESP_OK) {
        ESP_LOGE(TAG, "set_boot_partition failed: %s (image written+valid but not booted)",
                 esp_err_to_name(serr));
        api_reply_error(req, "500 Internal Server Error", "", "set boot failed");
        return ESP_FAIL;
    }
    /* Reboot whether or not the "ok" reaches the client — the image is already committed. The
       client will see the USB interface drop and come back; that is the update completing, not
       a failure. */
    if (api_reply_ok(req) != ESP_OK) ESP_LOGW(TAG, "resp send failed, rebooting anyway");
    ESP_LOGI(TAG, "OTA done - rebooting");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

esp_err_t ota_api_register(httpd_handle_t server)
{
    if (server == NULL) {
        ESP_LOGE(TAG, "no server to register on");
        return ESP_ERR_INVALID_ARG;
    }
    static const httpd_uri_t ota_uri = {
        .uri = DONGLE_PATH_OTA,
        .method = HTTP_POST,
        .handler = ota_post,
    };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &ota_uri), TAG,
                        "cannot register POST /ota");
    return ESP_OK;
}
