#include "video_enc.h"
#include "esp_log.h"
#include "esp_h264_enc_single_hw.h"
#include "contract.h"

static const char *TAG = "video_enc";

static esp_h264_enc_handle_t          s_enc;
static esp_h264_enc_param_hw_handle_t s_param;

esp_err_t video_enc_open(uint16_t bitrate_kbps) {
    if (s_enc) return ESP_ERR_INVALID_STATE;
    esp_h264_enc_cfg_hw_t cfg = {
        .pic_type = ESP_H264_RAW_FMT_O_UYY_E_VYY,   /* the only format rev < 3.0 accepts */
        .gop = VIDEO_FPS * VIDEO_KEYFRAME_S,
        .fps = VIDEO_FPS,
        .res = { .width = VIDEO_WIDTH, .height = VIDEO_HEIGHT },
        .rc = { .bitrate = (uint32_t)bitrate_kbps * 1000u,
                .qp_min = VIDEO_ENC_QP_MIN, .qp_max = VIDEO_ENC_QP_MAX },
    };
    if (esp_h264_enc_hw_new(&cfg, &s_enc) != ESP_H264_ERR_OK) {
        ESP_LOGE(TAG, "encoder: new failed");
        s_enc = NULL;
        return ESP_FAIL;
    }
    if (esp_h264_enc_hw_get_param_hd(s_enc, &s_param) != ESP_H264_ERR_OK ||
        esp_h264_enc_open(s_enc) != ESP_H264_ERR_OK) {
        ESP_LOGE(TAG, "encoder: open failed");
        esp_h264_enc_del(s_enc);
        s_enc = NULL;
        s_param = NULL;
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "encoder up: %dx%d @%d, gop %d, %u kbit/s, qp %d..%d",
             VIDEO_WIDTH, VIDEO_HEIGHT, VIDEO_FPS, VIDEO_FPS * VIDEO_KEYFRAME_S,
             (unsigned)bitrate_kbps, VIDEO_ENC_QP_MIN, VIDEO_ENC_QP_MAX);
    return ESP_OK;
}

esp_err_t video_enc_encode(uint8_t *yuv, size_t len, uint32_t pts_ms,
                           uint8_t *out, size_t out_cap, size_t *out_len, bool *keyframe) {
    if (!s_enc) return ESP_ERR_INVALID_STATE;
    esp_h264_enc_in_frame_t in = { .raw_data = { .buffer = yuv, .len = (uint32_t)len }, .pts = pts_ms };
    esp_h264_enc_out_frame_t o = { .raw_data = { .buffer = out, .len = (uint32_t)out_cap } };
    esp_h264_err_t e = esp_h264_enc_process(s_enc, &in, &o);
    if (e == ESP_H264_ERR_OVERFLOW || e == ESP_H264_ERR_MEM || e == ESP_H264_ERR_TIMEOUT) return ESP_ERR_INVALID_SIZE;
    if (e != ESP_H264_ERR_OK) return ESP_FAIL;
    *out_len = o.length;
    *keyframe = o.frame_type == ESP_H264_FRAME_TYPE_IDR;
    return ESP_OK;
}

void video_enc_force_idr(void) {
    if (s_param) esp_h264_enc_force_idr(&s_param->base);
}

void video_enc_close(void) {
    if (!s_enc) return;
    esp_h264_enc_close(s_enc);
    esp_h264_enc_del(s_enc);
    s_enc = NULL;
    s_param = NULL;
    ESP_LOGI(TAG, "encoder down");
}
