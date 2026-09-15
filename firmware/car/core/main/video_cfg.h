#ifndef VIDEO_CFG_H
#define VIDEO_CFG_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// The `video` domain of /config: the encoder's target bitrate, in kbit/s. Bounds here are
// the contract's (test_gen_contract's ranges test reads them from this file), and the value
// is read at stream start — video_link asks video_cfg_get_bitrate() when it opens the
// encoder, so a change lands on the next stream rather than mid-frame.
#define VIDEO_CFG_BITRATE_MIN     500
#define VIDEO_CFG_BITRATE_MAX     3000
#define VIDEO_CFG_BITRATE_DEFAULT 2500
#define VIDEO_CFG_ENABLED_DEFAULT true

// Load bitrate_kbps from NVS (default VIDEO_CFG_BITRATE_DEFAULT).
esp_err_t video_cfg_init(void);
// Clamped to the bounds. Returns false only when the lock could not be taken in time.
bool video_cfg_set_bitrate(uint16_t kbps);
uint16_t video_cfg_get_bitrate(void);
// Persist as {"bitrate_kbps":N} under the NVS key "video".
esp_err_t video_cfg_save(void);

#endif // VIDEO_CFG_H
