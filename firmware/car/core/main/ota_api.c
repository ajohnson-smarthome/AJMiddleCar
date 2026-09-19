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
#include "contract.h"
#include "ota_reply.h"

static const char *TAG = "ota_api";

/* ota_reply.h carries its own copy of the one IDF code it tells apart, so the map stays
   host-testable; this is where the copy meets the real header. */
_Static_assert(OTA_ERR_VALIDATE_FAILED == ESP_ERR_OTA_VALIDATE_FAILED,
               "ota_reply.h's ESP_ERR_OTA_VALIDATE_FAILED drifted from esp_ota_ops.h");

/* Send the envelope ota_reply_for chose for a refused esp_ota_* call. Every refusal is a
   reply and not ESP_FAIL — see the note at the first use. */
static esp_err_t ota_refuse(httpd_req_t *req, ota_step_t step, esp_err_t err) {
    ota_reply_t r = ota_reply_for(step, (int)err);
    return api_reply_error(req, r.status, r.code, "", r.msg);
}

static esp_err_t ota_post(httpd_req_t *req) {
    // Sticky: nothing may command the motors during a flash. Every failure path below
    // releases it again — a refused upload must not leave the car undriveable until
    // someone pulls the battery, because a failed flash leaves the running image intact.
    if (!car_stop(LINK_SRC_OTA)) {
        ESP_LOGE(TAG, "could not take the actuator for the flash — refusing the upload");
        return api_reply_error(req, "409 Conflict", ERR_BUSY, "", "actuator busy");
    }
    if (req->content_len < 4096) {  // reject obviously-bogus uploads before erasing a slot
        link_release_must(LINK_SRC_OTA);
        return api_reply_error(req, "400 Bad Request", ERR_TOO_SMALL, "", "image too small");
    }
    if (req->content_len > INT_MAX) {  // guard the (int) cast below: a huge len wraps negative
        link_release_must(LINK_SRC_OTA);
        return api_reply_error(req, "400 Bad Request", ERR_NOT_FIRMWARE, "", "image larger than the slot");
    }
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (part == NULL) {
        link_release_must(LINK_SRC_OTA);
        return api_reply_error(req, "500 Internal Server Error", ERR_WRITE_FAILED, "", "no ota partition");
    }
    if ((uint32_t)req->content_len > part->size) {
        link_release_must(LINK_SRC_OTA);
        return api_reply_error(req, "400 Bad Request", ERR_NOT_FIRMWARE, "", "image larger than the slot");
    }
    esp_ota_handle_t handle = 0;
    /* The exact length is known from Content-Length: erasing only what the image needs
       instead of OTA_SIZE_UNKNOWN's full 4 MB saves seconds of erase (and flash wear)
       per update — and a too-large image now fails here instead of after the erase. */
    esp_err_t err = esp_ota_begin(part, req->content_len, &handle);
    if (err != ESP_OK) {
        link_release_must(LINK_SRC_OTA);
        /* The reply's result and not ESP_FAIL, here and on every failure below. ESP_FAIL
           makes httpd close the session with the body unread, and TCP answers a close with
           unread data with RST — so the 500 that was just written never reached the client,
           which reported a connection error instead of the reason. On ESP_OK httpd purges
           the rest of the body first (httpd_req_delete), which for an upload rejected at
           esp_ota_begin means reading and discarding up to the whole image; that is the
           price of the client learning why. */
        return ota_refuse(req, OTA_STEP_BEGIN, err);
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
            // `internal` is a 500 by contract (docs/protocol.md): the client's bytes stopped
            // arriving, which is not a malformed request to be corrected and resent.
            return api_reply_error(req, "500 Internal Server Error", ERR_INTERNAL, "", "upload stalled");
        }
        timeouts = 0;  // progress resets the stall budget
        err = esp_ota_write(handle, buf, r);
        if (err != ESP_OK) {
            // The first block's header check lives inside esp_ota_write: by its code, a
            // body that is not an image (400) is told from a flash that refused (500).
            esp_ota_abort(handle);
            link_release_must(LINK_SRC_OTA);
            return ota_refuse(req, OTA_STEP_WRITE, err);
        }
        remaining -= r;
    }
    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        link_release_must(LINK_SRC_OTA);
        return ota_refuse(req, OTA_STEP_END, err);
    }
    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_boot_partition failed: %s (image written+valid but not booted)", esp_err_to_name(err));
        link_release_must(LINK_SRC_OTA);
        return ota_refuse(req, OTA_STEP_SET_BOOT, err);
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
    httpd_uri_t u = { .uri = PATH_OTA, .method = HTTP_POST, .handler = ota_post };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &u), TAG, "register " PATH_OTA);
    return ESP_OK;
}
