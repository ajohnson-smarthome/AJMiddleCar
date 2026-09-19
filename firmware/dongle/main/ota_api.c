#include "ota_api.h"

#include <limits.h>
#include <stdatomic.h>
#include <stdint.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "api_util.h"
#include "display.h"
#include "dongle_contract.inc"
#include "ota_reply.h"

static const char *TAG = "ota_api";

/* ota_reply.h carries its own copies of the two IDF codes it tells apart, so the map stays
   host-testable; this is where the copies meet the real header. */
_Static_assert(OTA_ERR_VALIDATE_FAILED == ESP_ERR_OTA_VALIDATE_FAILED,
               "ota_reply.h's ESP_ERR_OTA_VALIDATE_FAILED drifted from esp_ota_ops.h");
_Static_assert(OTA_ERR_ROLLBACK_INVALID_STATE == ESP_ERR_OTA_ROLLBACK_INVALID_STATE,
               "ota_reply.h's ESP_ERR_OTA_ROLLBACK_INVALID_STATE drifted from esp_ota_ops.h");

/* Bytes accepted, out of the upload's Content-Length — the same numbers the receive loop below
 * already computes for itself (remaining vs req->content_len), just also published for a reader
 * outside this file. s_ota_total is the "is one running at all" bit: httpd serves one request
 * at a time, so a zero here always means the last upload's cleanup ran, whether it finished,
 * failed, or was never even accepted past the size checks. */
static _Atomic uint32_t s_ota_done;
static _Atomic uint32_t s_ota_total;

/* Every return out of ota_post from esp_ota_begin onward calls this — including the failure
 * branches — so a request that dies partway through flashing cannot leave ota_api_progress
 * claiming an upload is still running. */
static void ota_progress_clear(void)
{
    atomic_store(&s_ota_total, 0);
    atomic_store(&s_ota_done, 0);
}

/* Send the envelope ota_reply_for chose for a refused esp_ota_* call, and hand back the
 * send's result. Every refusal in ota_post returns this or api_reply_error's result directly,
 * never ESP_FAIL: a handler that returns ESP_FAIL makes httpd close the session with the
 * body unread (httpd_uri.c: "Handler returns error, this socket should be closed" —
 * httpd_req_delete's purge never runs), and TCP answers a close with unread data with RST,
 * so the envelope just written never reached the client, which reported a connection error
 * instead of the reason. On ESP_OK httpd purges the rest of the body first
 * (httpd_req_delete), which for an upload refused at esp_ota_begin or on the first block
 * means reading and discarding up to the whole image; that is the price of the client
 * learning why. The car's twin paid it first (firmware/car/core/main/ota_api.c). */
static esp_err_t ota_refuse(httpd_req_t *req, ota_step_t step, esp_err_t err)
{
    ota_reply_t r = ota_reply_for(step, (int)err);
    return api_reply_error(req, r.status, r.code, "", r.msg);
}

/* A deliberate twin of firmware/car/core/main/ota_api.c, not a shared file — the two firmwares do not
 * reference each other. What is missing here is the car's actuator arbitration: the car seizes
 * the motors for the length of the flash and releases them on every failure path, because a
 * refused upload must not leave a car undriveable. The dongle has nothing that moves, so that
 * layer is absent rather than stubbed out. Every check that guards the flash itself is kept. */
static esp_err_t ota_post(httpd_req_t *req)
{
    if (req->content_len < 4096) {  /* reject obviously-bogus uploads before erasing a slot */
        return api_reply_error(req, "400 Bad Request", DONGLE_ERR_TOO_SMALL, "", "image too small");
    }
    if (req->content_len > INT_MAX) {  /* guard the (int) cast below: a huge len wraps negative */
        return api_reply_error(req, "400 Bad Request", DONGLE_ERR_NOT_FIRMWARE, "",
                               "image larger than the slot");
    }
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (part == NULL) {
        return api_reply_error(req, "500 Internal Server Error", DONGLE_ERR_WRITE_FAILED, "", "no ota partition");
    }
    if ((uint32_t)req->content_len > part->size) {
        return api_reply_error(req, "400 Bad Request", DONGLE_ERR_NOT_FIRMWARE, "",
                               "image larger than the slot");
    }

    esp_ota_handle_t handle = 0;
    /* The exact length is known from Content-Length: erasing only what the image needs, instead
       of OTA_SIZE_UNKNOWN's full 4 MB, saves seconds of erase and flash wear per update — and a
       too-large image fails above rather than after the erase. */
    esp_err_t err = esp_ota_begin(part, req->content_len, &handle);
    if (err != ESP_OK) {
        if (err == ESP_ERR_OTA_ROLLBACK_INVALID_STATE) {
            /* The running image is still PENDING_VERIFY, so IDF refuses to start another update.
               app_main cancels rollback before it returns, so reaching this needs a request that
               beat the last instruction of boot — which USB enumeration alone makes implausible.
               It gets its own log line anyway (and its own envelope, `busy`, in ota_reply_for):
               "ota begin failed" would send someone hunting the flash for a fault that is
               really a race. */
            ESP_LOGE(TAG, "refusing: this image has not finished verifying its own boot");
        } else {
            ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        }
        return ota_refuse(req, OTA_STEP_BEGIN, err);
    }
    ESP_LOGI(TAG, "OTA -> %s, %d bytes", part->label, (int)req->content_len);
    /* done first, total second -- the mirror of ota_progress_clear(), which stores total first.
     * Publishing total last means the field that gates the whole reading ("is one running at
     * all") is never visible before the counter it frames, so a reader cannot pair a new total
     * with the previous upload's leftover done. That leftover is zero today, because every
     * return past esp_ota_begin clears it, but this ordering does not depend on that coverage.
     * Setting done here rather than leaving it is what makes a reader see 0/total instead of a
     * one-chunk jump from whatever the last upload ended on. */
    atomic_store(&s_ota_done, 0);
    atomic_store(&s_ota_total, (uint32_t)req->content_len);

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
            ota_progress_clear();
            /* `internal` is a 500 by contract: the client's bytes stopped arriving, which is
               not a malformed request to be corrected and resent. The reply's result, as
               everywhere: httpd's purge of what is left will time out on the same silence and
               close the session then — after the envelope went out, not instead of it. */
            return api_reply_error(req, "500 Internal Server Error", DONGLE_ERR_INTERNAL, "", "upload stalled");
        }
        timeouts = 0;  /* progress resets the stall budget */
        /* esp_ota_write rejects a first byte that is not 0xE9 (ESP_IMAGE_HEADER_MAGIC) with
           ESP_ERR_OTA_VALIDATE_FAILED — the spec's "reject anything whose first byte is not the
           ESP image magic" is satisfied by IDF, not by a check of ours. Do not add a second one:
           a hand-rolled magic test would be a copy of app_update's that could drift from it. */
        err = esp_ota_write(handle, buf, r);
        if (err != ESP_OK) {
            /* The first block's header check lives inside esp_ota_write: by its code, a body
               that is not an image (400, the client's fault) is told from a flash that refused
               (500, this device's) — the map in ota_reply.h makes that call. */
            esp_ota_abort(handle);
            if (err != ESP_ERR_OTA_VALIDATE_FAILED) {
                ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            }
            ota_progress_clear();
            return ota_refuse(req, OTA_STEP_WRITE, err);
        }
        remaining -= r;
        atomic_fetch_add(&s_ota_done, (uint32_t)r);
    }

    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        ota_progress_clear();
        return ota_refuse(req, OTA_STEP_END, err);
    }
    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_boot_partition failed: %s (image written+valid but not booted)",
                 esp_err_to_name(err));
        ota_progress_clear();
        return ota_refuse(req, OTA_STEP_SET_BOOT, err);
    }
    /* Cleared here too, ahead of the reboot below: the image is already committed either way,
       but a client polling GET /status in the vTaskDelay window that follows should see the
       upload as finished, not as still running. */
    ota_progress_clear();
    /* Reboot whether or not the "ok" reaches the client — the image is already committed. The
       client will see the USB interface drop and come back; that is the update completing, not
       a failure. */
    if (api_reply_ok(req) != ESP_OK) ESP_LOGW(TAG, "resp send failed, rebooting anyway");
    ESP_LOGI(TAG, "OTA done - rebooting");
    display_reboot();   /* «Перезапуск» on the glass, not «Обновление» at 100 % through the reboot */
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

/* Bytes accepted of how many, while an upload is running. False when none is. */
bool ota_api_progress(uint32_t *done, uint32_t *total)
{
    /* done BEFORE total, because ota_progress_clear() stores them the other way round (total
     * first). Loading total first lets a clear land between the two loads and hand the caller a
     * nonzero total with a done of 0 -- «Обновление» at 0% with an empty gauge, for one frame,
     * at the exact end of a successful upload. This way round the interleaving yields total = 0
     * instead, and the zero check below turns it into "no upload running", which is true. */
    uint32_t d = atomic_load(&s_ota_done);
    uint32_t t = atomic_load(&s_ota_total);
    if (t == 0) return false;
    *total = t;
    *done  = d;
    return true;
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
