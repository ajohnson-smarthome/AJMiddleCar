#ifndef VIDEO_ENC_H
#define VIDEO_ENC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

// The hardware H.264 encoder, one stream at a time. Driven through esp_h264 directly
// rather than as the V4L2 M2M device, because force_idr — the whole recovery story — is
// not reachable through /dev/video11.
//
// The QP corridor is wide on purpose: esp_h264's rate control picks each frame's QP from
// the last one plus the accumulated error, clipped to [qp_min, qp_max]. esp_video's own
// defaults (25..26) leave it nowhere to go, and `bitrate` becomes decoration. These two
// are tuned on the bench (stage 2 of the spec) against `video.kbps`.
#define VIDEO_ENC_QP_MIN  20
#define VIDEO_ENC_QP_MAX  45
// Output buffer per frame. A keyframe of a noisy night scene at qp_min can be 150 KB+;
// the hardware reports overflow rather than truncating, and video_link drops the frame
// and forces an IDR when that happens.
#define VIDEO_ENC_OUT_MAX (256 * 1024)

esp_err_t video_enc_open(uint16_t bitrate_kbps);
// One frame in O_UYY_E_VYY (VIDEO_WIDTH x VIDEO_HEIGHT x 1.5 bytes) -> Annex B in `out`.
// ESP_ERR_INVALID_SIZE when the encoded frame did not fit `out_cap`.
esp_err_t video_enc_encode(uint8_t *yuv, size_t len, uint32_t pts_ms,
                           uint8_t *out, size_t out_cap, size_t *out_len, bool *keyframe);
void video_enc_force_idr(void);
void video_enc_close(void);

#endif // VIDEO_ENC_H
