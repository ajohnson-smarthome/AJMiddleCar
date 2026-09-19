#ifndef VIDEO_LINK_H
#define VIDEO_LINK_H

#include <stdint.h>
#include "esp_err.h"

// The video channel: one UDP socket on VIDEO_PORT, a subscription tied to the rt
// session's owner, and the stream toward whoever holds it. Three tasks, all below the
// actuator: video_ctl owns the socket's receive side and the subscription; video_enc
// owns the camera pipeline and the encoder and fills a six-slot ring in PSRAM; the sender
// drains the ring one chunk every 3 ms on an esp_timer's wake-up (the dongle's USB drains
// ~4 Mbit/s), so a keyframe leaves as a trickle rather than a burst. Nothing here touches
// the motors.
//
// Call after camera_init() and rt_link_start(). A car whose camera is off still starts
// this: the socket answers nothing, and video.state says why.
esp_err_t video_link_start(void);

typedef struct {
    const char *state;     // VIDEO_STATE_OFF / IDLE / STREAMING — off is no sensor at boot,
                           // or a sensor that went quiet while watched: VIDEO_RETRY_OFF_AFTER
                           // starts in a row without a frame (video_retry.h), until one arrives
    uint32_t fps;          // frames sent in the last second — counted at the sender's output,
                           // when a frame's last chunk leaves, not at the encoder
    uint32_t kbps;         // kbit sent in the last second
    uint32_t dropped;      // frames not sent since boot
} video_link_stats_t;

// For telemetry, from any task: aligned u32 loads, no lock.
void video_link_stats(video_link_stats_t *out);

#endif // VIDEO_LINK_H
