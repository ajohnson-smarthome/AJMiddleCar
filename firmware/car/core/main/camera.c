#include "camera.h"
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_video_init.h"
#include "esp_video_device.h"
#include "esp_video_ioctl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "linux/videodev2.h"
#include "board.h"
#include "contract.h"
#include "i2c_bus.h"

static const char *TAG = "camera";

#define CAM_BUFFERS 3   /* one being filled, one being encoded, one in hand */

static bool s_present;
static int  s_fd = -1;
static uint8_t *s_buf[CAM_BUFFERS];
static size_t   s_buf_len[CAM_BUFFERS];
/* Serialises camera_start and camera_stop (camera.h says why). Created in camera_init,
   before any caller exists; the fast paths that only read s_fd stay outside it. */
static SemaphoreHandle_t s_lock;

esp_err_t camera_init(void) {
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        ESP_LOGW(TAG, "no mutex — camera off");
        return ESP_OK;
    }
    i2c_master_bus_handle_t bus = i2c_bus_handle();
    if (bus == NULL) {
        ESP_LOGW(TAG, "no I2C bus — camera off");
        return ESP_OK;
    }
    esp_video_init_csi_config_t csi = {
        .sccb_config = { .init_sccb = false, .i2c_handle = bus, .freq = BOARD_SCCB_HZ },
        .reset_pin = BOARD_CAM_RESET_PIN,
        .pwdn_pin  = BOARD_CAM_PWDN_PIN,
    };
    esp_video_init_config_t cfg = { .csi = &csi };
    esp_err_t err = esp_video_init(&cfg);
    if (err != ESP_OK) {
        /* Not a boot failure: a car without a camera is still the car. Logged once, and
           reported as video.state "off" for as long as this boot lasts. */
        ESP_LOGW(TAG, "no camera: esp_video_init %s", esp_err_to_name(err));
        return ESP_OK;
    }
    s_present = true;
    ESP_LOGI(TAG, "sensor up on %s, %dx%d", ESP_VIDEO_MIPI_CSI_DEVICE_NAME, VIDEO_WIDTH, VIDEO_SENSOR_HEIGHT);
    return ESP_OK;
}

bool camera_present(void) { return s_present; }
bool camera_running(void) { return s_fd >= 0; }

size_t camera_frame_bytes(camera_fmt_t fmt) {
    size_t px = (size_t)VIDEO_WIDTH * VIDEO_SENSOR_HEIGHT;
    return fmt == CAMERA_FMT_YUV420 ? px * 3 / 2 : px * 2;
}

static void unmap_all(void) {
    for (int i = 0; i < CAM_BUFFERS; i++) {
        if (s_buf[i]) munmap(s_buf[i], s_buf_len[i]);
        s_buf[i] = NULL;
    }
}

/* The body of camera_start, under s_lock. Every early return here unwinds through the
   wrapper below, so none of them can forget the give. */
static esp_err_t start_locked(camera_fmt_t fmt) {
    if (s_fd >= 0) return ESP_ERR_INVALID_STATE;

    int fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDONLY);
    ESP_RETURN_ON_FALSE(fd >= 0, ESP_FAIL, TAG, "open %s", ESP_VIDEO_MIPI_CSI_DEVICE_NAME);

    /* Bounds camera_acquire's DQBUF: esp_video's VFS has no select(), and the default
       wait is portMAX_DELAY, which would let a CSI that never delivers a frame wedge
       whichever task calls camera_acquire — the httpd task today, the encode task once
       streaming exists. 500 ms is ~22 frame periods at 45 fps, comfortably past a single
       dropped frame without being mistaken for progress. */
    struct timeval tv = { .tv_sec = 0, .tv_usec = 500000 };
    if (ioctl(fd, VIDIOC_S_DQBUF_TIMEOUT, &tv) != 0) {
        close(fd);
        ESP_LOGE(TAG, "S_DQBUF_TIMEOUT");
        return ESP_FAIL;
    }

    const int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    struct v4l2_format format = {
        .type = type,
        .fmt.pix.width = VIDEO_WIDTH,
        .fmt.pix.height = VIDEO_SENSOR_HEIGHT,   /* the whole frame; the stream crops it (frame_crop.h) */
        .fmt.pix.pixelformat = fmt == CAMERA_FMT_YUV420 ? V4L2_PIX_FMT_YUV420 : V4L2_PIX_FMT_UYVY,
    };
    if (ioctl(fd, VIDIOC_S_FMT, &format) != 0) { close(fd); ESP_LOGE(TAG, "S_FMT"); return ESP_FAIL; }

    struct v4l2_requestbuffers req = { .count = CAM_BUFFERS, .type = type, .memory = V4L2_MEMORY_MMAP };
    if (ioctl(fd, VIDIOC_REQBUFS, &req) != 0) { close(fd); ESP_LOGE(TAG, "REQBUFS"); return ESP_FAIL; }

    for (int i = 0; i < CAM_BUFFERS; i++) {
        struct v4l2_buffer buf = { .type = type, .memory = V4L2_MEMORY_MMAP, .index = i };
        if (ioctl(fd, VIDIOC_QUERYBUF, &buf) != 0) { unmap_all(); close(fd); return ESP_FAIL; }
        s_buf[i] = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.offset);
        s_buf_len[i] = buf.length;
        if (!s_buf[i]) { unmap_all(); close(fd); return ESP_ERR_NO_MEM; }
        if (ioctl(fd, VIDIOC_QBUF, &buf) != 0) { unmap_all(); close(fd); return ESP_FAIL; }
    }
    if (ioctl(fd, VIDIOC_STREAMON, &type) != 0) { unmap_all(); close(fd); ESP_LOGE(TAG, "STREAMON"); return ESP_FAIL; }
    s_fd = fd;
    ESP_LOGI(TAG, "pipeline up (%s)", fmt == CAMERA_FMT_YUV420 ? "yuv420" : "uyvy");
    return ESP_OK;
}

esp_err_t camera_start(camera_fmt_t fmt) {
    if (!s_present) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = start_locked(fmt);
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t camera_stop(void) {
    if (!s_present) return ESP_OK;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_fd >= 0) {
        const int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        /* STREAMOFF is what puts the sensor in standby (the driver writes its stream
           register) and parks the isp_task on an empty statistics queue. */
        if (ioctl(s_fd, VIDIOC_STREAMOFF, &type) != 0) ESP_LOGW(TAG, "STREAMOFF failed");
        unmap_all();
        close(s_fd);
        s_fd = -1;
        ESP_LOGI(TAG, "pipeline down");
    }
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t camera_acquire(camera_frame_t *out) {
    if (s_fd < 0) return ESP_ERR_INVALID_STATE;
    struct v4l2_buffer buf = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP };
    if (ioctl(s_fd, VIDIOC_DQBUF, &buf) != 0) return ESP_FAIL;
    out->data = s_buf[buf.index];
    out->len = buf.bytesused;
    out->index = buf.index;
    out->captured_ms = (uint32_t)(esp_timer_get_time() / 1000);
    return ESP_OK;
}

esp_err_t camera_release(const camera_frame_t *f) {
    if (s_fd < 0) return ESP_ERR_INVALID_STATE;
    struct v4l2_buffer buf = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP, .index = f->index };
    return ioctl(s_fd, VIDIOC_QBUF, &buf) == 0 ? ESP_OK : ESP_FAIL;
}
