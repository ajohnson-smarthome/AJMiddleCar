#include "snapshot_api.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_server.h"
#include "driver/jpeg_encode.h"
#include "api_util.h"
#include "camera.h"
#include "contract.h"
#include "http_server.h"

static const char *TAG = "snapshot";

/* How long to run the pipeline before the frame that is kept. The sensor's first frames
   after standby are dark, and AE walks up from its cold start over several statistics
   rounds: measured from a stream on the bench (2026-09-14), the picture is black at 0 s and
   settled by ~2.7 s. Ten frames (~220 ms) — the spec's half-second budget — returned a
   frame of mean brightness 1.8/255, every time. A snapshot is a bench tool, so it waits
   the three seconds instead of being fast and black. Time, not a frame count: the bound
   should not move with the sensor mode's frame rate. */
#define SNAPSHOT_WARMUP_MS 3000
#define SNAPSHOT_QUALITY 80
#define SNAPSHOT_OUT_MAX (512 * 1024)

static jpeg_encoder_handle_t s_jpeg;

static esp_err_t snapshot_get(httpd_req_t *req) {
    /* Declared up front: camera_start's failure now joins the same 500 reply the rest of
       the capture uses, via `failed:` (R5 — ESP_RETURN_ON_ERROR returning straight out of
       an httpd handler skips the 500 envelope and esp_http_server just resets the socket),
       and a goto is not allowed to jump forward past a declaration it might then use. */
    esp_err_t err = ESP_OK;
    camera_frame_t f;
    uint8_t *jpg = NULL;
    size_t cap = 0;

    if (!camera_present()) {
        return api_reply_error(req, "500 Internal Server Error", ERR_INTERNAL, "", "camera off");
    }
    /* Someone else — the stream — owns the pipeline. On rev 1.3 the JPEG block cannot take
       the stream's YUV420 buffers, and re-laying 1.8 MB for a bench route is not worth it. */
    if (camera_running()) {
        return api_reply_error(req, "409 Conflict", ERR_BUSY, "", "stream running");
    }
    err = camera_start(CAMERA_FMT_UYVY);
    /* The check above and this start are not atomic against the stream's own start — the
       start itself is (camera.h's lock), which is what makes losing the race a clean
       INVALID_STATE rather than two owners of one fd. It must answer here rather than at
       `out:`, whose camera_stop() would STREAMOFF and close the stream's fd under the
       encoder (R9a). */
    if (err == ESP_ERR_INVALID_STATE) {
        return api_reply_error(req, "409 Conflict", ERR_BUSY, "", "stream running");
    }
    /* Any other failure started nothing — every failure path inside camera_start closes
       and unmaps its own fd — so this must not reach `out:` either: its camera_stop() runs
       after the lock was released, and a stream that started in that gap would be the one
       it stopped. `failed:` is the same reply, past the stop. */
    if (err != ESP_OK) goto failed;

    for (int64_t t0 = esp_timer_get_time(); esp_timer_get_time() - t0 < SNAPSHOT_WARMUP_MS * 1000LL;) {
        if ((err = camera_acquire(&f)) != ESP_OK) goto out;
        camera_release(&f);
    }
    if ((err = camera_acquire(&f)) != ESP_OK) goto out;

    jpeg_encode_memory_alloc_cfg_t mem = { .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER };
    jpg = jpeg_alloc_encoder_mem(SNAPSHOT_OUT_MAX, &mem, &cap);
    if (!jpg) { camera_release(&f); err = ESP_ERR_NO_MEM; goto out; }

    jpeg_encode_cfg_t cfg = {
        .width = VIDEO_WIDTH, .height = VIDEO_HEIGHT,
        .src_type = JPEG_ENCODE_IN_FORMAT_YUV422,   /* UYVY from the ISP, as Espressif's own example maps it */
        .sub_sample = JPEG_DOWN_SAMPLING_YUV422,
        .image_quality = SNAPSHOT_QUALITY,
    };
    uint32_t size = 0;
    err = jpeg_encoder_process(s_jpeg, &cfg, f.data, (uint32_t)f.len, jpg, (uint32_t)cap, &size);
    camera_release(&f);
    camera_stop();
    if (err != ESP_OK) {
        heap_caps_free(jpg);
        ESP_LOGE(TAG, "jpeg: %s", esp_err_to_name(err));
        return api_reply_error(req, "500 Internal Server Error", ERR_INTERNAL, "", "jpeg failed");
    }
    httpd_resp_set_type(req, "image/jpeg");
    err = httpd_resp_send(req, (const char *)jpg, (ssize_t)size);
    heap_caps_free(jpg);
    return err;

out:
    /* Only the paths where this handler started the pipeline arrive here. */
    camera_stop();
failed:
    ESP_LOGE(TAG, "capture: %s", esp_err_to_name(err));
    return api_reply_error(req, "500 Internal Server Error", ERR_INTERNAL, "", "capture failed");
}

esp_err_t snapshot_api_start(void) {
    httpd_handle_t server = http_server_get_handle();
    if (server == NULL) return ESP_FAIL;
    jpeg_encode_engine_cfg_t eng = { .timeout_ms = 1000 };
    ESP_RETURN_ON_ERROR(jpeg_new_encoder_engine(&eng, &s_jpeg), TAG, "jpeg engine");
    httpd_uri_t uri = { .uri = PATH_SNAPSHOT, .method = HTTP_GET, .handler = snapshot_get };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &uri), TAG, "reg GET " PATH_SNAPSHOT);
    return ESP_OK;
}
