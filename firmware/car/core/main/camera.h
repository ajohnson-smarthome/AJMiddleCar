#ifndef CAMERA_H
#define CAMERA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

// The sensor and the capture pipeline, as the rest of the firmware sees them.
//
// camera_init runs once at boot: it brings up esp_video on the shared I2C bus and detects
// the sensor. No sensor is not an error — the car drives without one — it is the `off`
// state, and every later call answers ESP_ERR_INVALID_STATE.
//
// The pipeline (CSI DMA, ISP, the component's isp_task) runs only between camera_start
// and camera_stop. Stopped, the sensor is in standby and nothing touches PSRAM or the
// I2C bus on the camera's behalf: that is what `idle` means on the wire, and it is why a
// firmware update does not need to know about the camera at all.
esp_err_t camera_init(void);
bool camera_present(void);

// What the ISP writes into the frame buffers. YUV420 here is the P4's O_UYY_E_VYY packing
// (odd rows U Y Y U Y Y…, even rows V Y Y…), the one layout the rev 1.3 H.264 block
// accepts — not planar I420. UYVY is for the JPEG block, which on rev 1.3 takes no 4:2:0.
typedef enum { CAMERA_FMT_YUV420, CAMERA_FMT_UYVY } camera_fmt_t;

// Bytes per frame at VIDEO_WIDTH x VIDEO_SENSOR_HEIGHT — the sensor's whole frame — in the given format.
size_t camera_frame_bytes(camera_fmt_t fmt);

// Start the pipeline in `fmt` (ESP_ERR_INVALID_STATE if running or absent), stop it.
//
// Serialised against each other by a mutex, held across the whole of either. Two tasks
// reach for the same device — the httpd task for a snapshot, the encode task for the
// stream — and esp_video reference-counts opens rather than refusing a second one, so
// without the lock two starts interleaved at the open could both "succeed" on one fd and
// the loser's stop would STREAMOFF the winner's pipeline. The lock is what makes "running"
// a single answer: the second start sees s_fd set and gets INVALID_STATE. camera_running
// itself is a lock-free read, good enough for the snapshot's early 409 — the start that
// follows it is the check that counts.
esp_err_t camera_start(camera_fmt_t fmt);
esp_err_t camera_stop(void);
bool camera_running(void);

// One frame, driver-owned, valid until camera_release. The wait is bounded to 500 ms —
// ~22 frame periods at 45 fps — set as the DQBUF timeout in camera_start: esp_video's VFS
// has no select(), so a device timeout is the only way to bound the wait instead of
// polling, and it is what keeps a CSI that never delivers a frame from wedging the httpd
// task or, later, the encode task. The caller is still expected to keep the pipeline
// alive and check its own stop flag between frames. Not under the start/stop lock: only
// the task that started the pipeline calls these, and holding a mutex across a DQBUF
// that may wait 500 ms would stall a stop for that long.
typedef struct {
    uint8_t *data;
    size_t   len;
    uint32_t index;        // the V4L2 buffer index, handed back in camera_release
    uint32_t captured_ms;  // boot-relative, stamped at dequeue
} camera_frame_t;

esp_err_t camera_acquire(camera_frame_t *out);
esp_err_t camera_release(const camera_frame_t *f);

#endif // CAMERA_H
