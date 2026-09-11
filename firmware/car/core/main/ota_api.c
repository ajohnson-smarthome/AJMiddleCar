#include "ota_api.h"
#include <string.h>
#include <limits.h>
#include "esp_http_server.h"
#include "esp_check.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "http_server.h"
#include "car.h"
#include "link.h"
#include "api_util.h"

static const char *TAG = "ota_api";

static esp_err_t ota_post(httpd_req_t *req) {
    // Sticky: nothing may command the motors during a flash. Every failure path below
    // releases it again — a refused upload must not leave the car undriveable until
    // someone pulls the battery, because a failed flash leaves the running image intact.
    if (!car_stop(LINK_SRC_OTA)) {
        ESP_LOGE(TAG, "could not take the actuator for the flash — refusing the upload");
        return api_reply_error(req, "500 Internal Server Error", "", "actuator busy");
    }
    if (req->content_len < 4096) {  // reject obviously-bogus uploads before erasing a slot
        link_release_must(LINK_SRC_OTA);
        return api_reply_error(req, "400 Bad Request", "", "image too small");
    }
    if (req->content_len > INT_MAX) {  // guard the (int) cast below: a huge len wraps negative
        link_release_must(LINK_SRC_OTA);
        return api_reply_error(req, "400 Bad Request", "", "image too large");
    }
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (part == NULL) {
        link_release_must(LINK_SRC_OTA);
        api_reply_error(req, "500 Internal Server Error", "", "no ota partition");
        return ESP_FAIL;
    }
    if ((uint32_t)req->content_len > part->size) {
        link_release_must(LINK_SRC_OTA);
        return api_reply_error(req, "400 Bad Request", "", "image too large");
    }
    esp_ota_handle_t handle = 0;
    /* The exact length is known from Content-Length: erasing only what the image needs
       instead of OTA_SIZE_UNKNOWN's full 4 MB saves seconds of erase (and flash wear)
       per update — and a too-large image now fails here instead of after the erase. */
    if (esp_ota_begin(part, req->content_len, &handle) != ESP_OK) {
        link_release_must(LINK_SRC_OTA);
        api_reply_error(req, "500 Internal Server Error", "", "ota begin failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "OTA -> %s, %d bytes", part->label, (int)req->content_len);

    char buf[1024];
    int remaining = (int)req->content_len;
    int timeouts = 0;  // bound stalls: a silent client must not wedge the single httpd task forever
    while (remaining > 0) {
        int chunk = remaining < (int)sizeof(buf) ? remaining : (int)sizeof(buf);
        int r = httpd_req_recv(req, buf, chunk);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts <= 6) continue;  // ~6×5s grace, then abort
            esp_ota_abort(handle);
            link_release_must(LINK_SRC_OTA);
            api_reply_error(req, "400 Bad Request", "", "recv error");
            return ESP_FAIL;
        }
        timeouts = 0;  // progress resets the stall budget
        if (esp_ota_write(handle, buf, r) != ESP_OK) {
            esp_ota_abort(handle);
            link_release_must(LINK_SRC_OTA);
            api_reply_error(req, "500 Internal Server Error", "", "ota write failed");
            return ESP_FAIL;
        }
        remaining -= r;
    }
    if (esp_ota_end(handle) != ESP_OK) {
        link_release_must(LINK_SRC_OTA);
        api_reply_error(req, "400 Bad Request", "", "image invalid");
        return ESP_FAIL;
    }
    esp_err_t berr = esp_ota_set_boot_partition(part);
    if (berr != ESP_OK) {
        ESP_LOGE(TAG, "set_boot_partition failed: %s (image written+valid but not booted)", esp_err_to_name(berr));
        link_release_must(LINK_SRC_OTA);
        api_reply_error(req, "500 Internal Server Error", "", "set boot failed");
        return ESP_FAIL;
    }
    // Reboot regardless of whether the "ok" reaches the client — the image is already committed.
    if (api_reply_ok(req) != ESP_OK) ESP_LOGW(TAG, "resp send failed, rebooting anyway");
    ESP_LOGI(TAG, "OTA done - rebooting");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

esp_err_t ota_api_start(void) {
    httpd_handle_t server = http_server_get_handle();
    if (server == NULL) { ESP_LOGE(TAG, "http server not started"); return ESP_FAIL; }
    httpd_uri_t u = { .uri = "/ota", .method = HTTP_POST, .handler = ota_post };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &u), TAG, "register /ota");
    return ESP_OK;
}
