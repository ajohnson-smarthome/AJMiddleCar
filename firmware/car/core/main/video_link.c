#include "video_link.h"
#include <errno.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "esp_h264_alloc.h"
#include "camera.h"
#include "contract.h"
#include "control_proto.h"
#include "frame_crop.h"
#include "rt_link.h"
#include "video_cfg.h"
#include "video_enc.h"
#include "video_sub.h"
#include "video_wire.h"

static const char *TAG = "video";

#define CTL_TICK_MS      100                          /* recvfrom timeout: the subscription clock */
/* Six, not two: at SEND_PERIOD_US a 60-chunk keyframe (the old 1280x960 one; ~20 chunks
   since the crop to 720 rows) takes ~180 ms to leave, several encoded frames' worth, and
   the P-frames right after an IDR are the heavy ones. A ring of
   two dropped the frame behind every keyframe; a ring of three still dropped about one a
   second under motion — and every drop forces an IDR, which is another 130 ms burst, which
   drops another frame: the bench watched the bitrate climb to 2.9 Mbit/s on a 1.5 target
   from nothing but that loop. Slots are 256 KB each in PSRAM, of which there are 32 MB. */
#define RING_SLOTS       6
/* Encode every FRAME_SKIP-th sensor frame. The contract's fps is the nominal rate — the
   floor of the real one, 45/2 = 22.5 told to the app and the encoder's rate control as 22 —
   so the skip is the integer quotient and the assert below is that it lands on that floor
   (a 30 in the contract would silently become 45 otherwise). */
#define FRAME_SKIP       (VIDEO_SENSOR_FPS / VIDEO_FPS)
/* The encoder sees the middle VIDEO_HEIGHT rows of the sensor's frame — frame_crop.h. */
#define CROP_OFFSET      frame_crop_offset(VIDEO_WIDTH, VIDEO_SENSOR_HEIGHT, VIDEO_HEIGHT)
#define CROP_BYTES       frame_crop_bytes(VIDEO_WIDTH, VIDEO_HEIGHT)
/* One chunk every 3 ms — 3.7 Mbit/s while a frame is leaving. The dongle's USB is
   Full-Speed and its host takes one transfer block at a time, ~5 ms between reads plus
   ~1.2 ms of wire per chunk, so what it drains depends on how many chunks land in each
   block: about 4 Mbit/s at this spacing, 6 with denser bursts, and a block that overruns
   the S3's 127-packet limit hangs the link (tinyusb#3825 — capped on the dongle since).
   3.7 offered against ~4 drained means a run of heavy frames cannot overflow the dongle's
   ring; a 60-chunk keyframe leaves in ~180 ms, which the six-slot ring here absorbs. Bench
   2026-09-16 with the real stream: 1 ms lost 49 %, 4 ms was clean but starved the blocks,
   2 and 3 ms both 99–100 % whole at 1500–2500 kbit/s; 3 keeps the margin. With the 720-row
   crop (2026-09-15, bringup.md) a keyframe is ~20 chunks and 2500–3000 kbit/s arrive whole. */
#define SEND_PERIOD_US   3000

_Static_assert(VIDEO_SENSOR_FPS / FRAME_SKIP == VIDEO_FPS, "fps must be the floor of the sensor rate over a whole skip");
_Static_assert(VIDEO_FPS * VIDEO_KEYFRAME_S <= 255, "the hardware encoder's GOP is at most 255");
_Static_assert(VIDEO_HEIGHT <= VIDEO_SENSOR_HEIGHT, "the wire picture is a crop of the sensor's frame");

/* ---- shared state ---------------------------------------------------------------- */
static int s_sock = -1;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static struct sockaddr_in s_peer;          /* under s_mux */
static volatile bool s_want;               /* ctl -> enc: stream wanted */
static volatile bool s_streaming;          /* enc: pipeline and encoder are up */
static volatile bool s_force_idr;          /* ctl/enc -> enc */
static volatile uint32_t s_fps, s_kbps, s_dropped;

/* One encoded frame, waiting to be sent. The buffer IS the encoder's output buffer, so a
   frame is never copied: the encoder writes into a free slot, the sender drains it. */
typedef struct {
    uint8_t *buf;
    size_t   len;
    unsigned next_chunk, n_chunks;
    vw_header_t hdr;
    /* The publication point: the encoder fills the fields, then sets this; the sender
       reads this, then the fields. volatile alone orders only volatile accesses, and on
       two cores nothing else would keep the plain stores ahead of the flag — so the flag
       is written with a release store and read with an acquire load (R8). */
    volatile bool full;
} ring_slot_t;
static ring_slot_t s_ring[RING_SLOTS];
static unsigned s_ring_head;               /* sender reads here */
static unsigned s_ring_tail;               /* encoder writes here */
static uint8_t  s_stream;                  /* +1 per start */
static TaskHandle_t s_sender;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void log_peer(const char *what, const struct sockaddr_in *p) {
    char ip[16];
    inet_ntoa_r(p->sin_addr, ip, sizeof(ip));
    ESP_LOGI(TAG, "%s %s:%u", what, ip, (unsigned)ntohs(p->sin_port));
}

/* ---- sender: one chunk per wake-up ------------------------------------------------ */
static void sender_wake(void *arg) { (void)arg; if (s_sender) xTaskNotifyGive(s_sender); }

static void sender_task(void *arg) {
    (void)arg;
    uint8_t dgram[VIDEO_HEADER_BYTES + VIDEO_CHUNK_BYTES];
    uint32_t sec_start = now_ms(), sec_bytes = 0, sec_frames = 0;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
        uint32_t t = now_ms();
        if ((uint32_t)(t - sec_start) >= 1000) {
            s_kbps = sec_bytes * 8 / 1000;
            s_fps = sec_frames;
            sec_start = t; sec_bytes = 0; sec_frames = 0;
        }
        ring_slot_t *slot = &s_ring[s_ring_head];
        if (!__atomic_load_n(&slot->full, __ATOMIC_ACQUIRE)) continue;
        size_t d = vw_chunk(&slot->hdr, slot->buf, slot->len, slot->next_chunk, dgram);
        if (d > 0) {
            struct sockaddr_in to;
            taskENTER_CRITICAL(&s_mux);
            to = s_peer;
            taskEXIT_CRITICAL(&s_mux);
            if (sendto(s_sock, dgram, d, 0, (struct sockaddr *)&to, sizeof(to)) < 0) {
                static uint32_t last_log = (uint32_t)-1001;
                if ((uint32_t)(t - last_log) > 1000) { last_log = t; ESP_LOGW(TAG, "sendto: errno %d", errno); }
            } else {
                sec_bytes += d;
            }
        }
        if (++slot->next_chunk >= slot->n_chunks) {
            __atomic_store_n(&slot->full, false, __ATOMIC_RELEASE);
            sec_frames++;
            s_ring_head = (s_ring_head + 1) % RING_SLOTS;
        }
    }
}

/* ---- encoder task: camera -> H.264 -> ring ---------------------------------------- */
static bool stream_open(void) {
    ESP_RETURN_ON_FALSE(camera_start(CAMERA_FMT_YUV420) == ESP_OK, false, TAG, "camera start");
    if (video_enc_open(video_cfg_get_bitrate()) != ESP_OK) { camera_stop(); return false; }
    s_stream++;
    for (unsigned i = 0; i < RING_SLOTS; i++) s_ring[i].full = false;
    s_ring_head = s_ring_tail = 0;
    s_streaming = true;
    ESP_LOGI(TAG, "stream %u up", s_stream);
    return true;
}

static void stream_close(void) {
    s_streaming = false;
    video_enc_close();
    camera_stop();
    for (unsigned i = 0; i < RING_SLOTS; i++) s_ring[i].full = false;
    s_fps = 0; s_kbps = 0;
    ESP_LOGI(TAG, "stream %u down", s_stream);
}

static void enc_task(void *arg) {
    (void)arg;
    uint16_t frame = 0;
    uint32_t sensor_frames = 0;
    for (;;) {
        if (!s_want) { vTaskDelay(pdMS_TO_TICKS(CTL_TICK_MS)); continue; }
        if (!stream_open()) { vTaskDelay(pdMS_TO_TICKS(1000)); continue; }
        frame = 0; sensor_frames = 0;
        bool capture_failed = false;
        while (s_want) {
            camera_frame_t f;
            if (camera_acquire(&f) != ESP_OK) { ESP_LOGE(TAG, "capture failed — stream over"); capture_failed = true; break; }
            bool take = (sensor_frames++ % FRAME_SKIP) == 0;
            if (!take) { camera_release(&f); continue; }
            ring_slot_t *slot = &s_ring[s_ring_tail];
            if (__atomic_load_n(&slot->full, __ATOMIC_ACQUIRE)) {
                /* The sender is behind: this sensor frame is skipped BEFORE the encoder sees
                   it, so the encoder's reference is still the last frame it encoded — which
                   the ring holds and the sender will deliver — and `frame` below does not
                   advance, so the receiver sees no gap either. Nothing to repair: the cost is
                   one frame of motion, a momentarily lower fps. This used to force an IDR
                   too, and that was the amplifier of a loop the bench watched at 2 ms pacing:
                   a keyframe backs the ring up → a skip → a forced IDR (another 84 KB) →
                   another skip → 2.9 Mbit/s on a 1.5 target and the USB behind the dongle
                   overrun. The forced IDR belongs only to the branch below, where the encoder
                   DID consume a frame as a reference and nothing was sent. */
                camera_release(&f);
                s_dropped++;
                continue;
            }
            if (s_force_idr) { s_force_idr = false; video_enc_force_idr(); }
            size_t len = 0; bool key = false;
            esp_err_t err = video_enc_encode(f.data + CROP_OFFSET, CROP_BYTES, f.captured_ms, slot->buf, VIDEO_ENC_OUT_MAX, &len, &key);
            camera_release(&f);
            if (err != ESP_OK || vw_chunk_count(len) == 0) {
                s_dropped++;
                s_force_idr = true;   /* whatever did not fit, the decoder has not seen */
                continue;
            }
            slot->len = len;
            slot->n_chunks = vw_chunk_count(len);
            slot->next_chunk = 0;
            slot->hdr = (vw_header_t){ .proto = VIDEO_WIRE_PROTO, .flags = key ? VW_FLAG_KEY : 0,
                                       .stream = s_stream, .frame = frame++, .captured_ms = f.captured_ms };
            __atomic_store_n(&slot->full, true, __ATOMIC_RELEASE);
            s_ring_tail = (s_ring_tail + 1) % RING_SLOTS;
        }
        stream_close();
        /* A sensor that stopped delivering would otherwise be reopened at ~2 Hz (R9b). */
        if (capture_failed) vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* ---- control task: the socket and the subscription -------------------------------- */
static void ctl_task(void *arg) {
    (void)arg;
    video_sub_t sub;
    video_sub_init(&sub);
    char buf[RT_MAX_COMMAND + 1];
    for (;;) {
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        int n = recvfrom(s_sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &flen);
        char owner[CONTROL_SID_MAX];
        rt_link_owner_sid(owner);
        uint32_t t = now_ms();
        if (n > 0 && camera_present()) {
            control_frame_t f;
            if (control_parse_frame(buf, (size_t)n, RT_MAX_COMMAND, &f) == 0) {
                bool idr = false;
                switch (video_sub_view(&sub, owner, &f, t, &idr)) {
                case VS_START:
                    taskENTER_CRITICAL(&s_mux); s_peer = from; taskEXIT_CRITICAL(&s_mux);
                    /* Harmless on the fresh encoder this usually meets, and load-bearing on
                       the one it sometimes does not: a view that lands after the encode
                       loop's `while (s_want)` gave up but before it re-read the flag keeps
                       the old stream going, and its first frame would be a P-frame the new
                       viewer cannot decode. */
                    s_force_idr = true;
                    s_want = true;
                    log_peer("view from", &from);
                    break;
                case VS_REFRESH: {
                    struct sockaddr_in cur;
                    taskENTER_CRITICAL(&s_mux); cur = s_peer; taskEXIT_CRITICAL(&s_mux);
                    if (cur.sin_addr.s_addr != from.sin_addr.s_addr || cur.sin_port != from.sin_port) {
                        /* Same sid, new address: the phone re-made its socket, or someone who
                           knows the sid took the stream. Both are allowed; both are said. */
                        taskENTER_CRITICAL(&s_mux); s_peer = from; taskEXIT_CRITICAL(&s_mux);
                        log_peer("viewer moved to", &from);
                    }
                    if (idr) s_force_idr = true;
                    break;
                }
                case VS_IGNORE:
                    break;
                }
            }
        }
        if (video_sub_expired(&sub, owner[0] != '\0', t)) {
            video_sub_end(&sub);
            s_want = false;
            ESP_LOGI(TAG, "viewer gone — stream ends");
        }
    }
}

/* ---- public ----------------------------------------------------------------------- */
void video_link_stats(video_link_stats_t *out) {
    out->state = !camera_present() ? VIDEO_STATE_OFF : (s_streaming ? VIDEO_STATE_STREAMING : VIDEO_STATE_IDLE);
    out->fps = s_fps;
    out->kbps = s_kbps;
    out->dropped = s_dropped;
}

esp_err_t video_link_start(void) {
    for (unsigned i = 0; i < RING_SLOTS; i++) {
        uint32_t got = 0;
        s_ring[i].buf = esp_h264_aligned_calloc(16, 1, VIDEO_ENC_OUT_MAX, &got, MALLOC_CAP_SPIRAM | MALLOC_CAP_CACHE_ALIGNED);
        ESP_RETURN_ON_FALSE(s_ring[i].buf, ESP_ERR_NO_MEM, TAG, "ring slot %u", i);
    }
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ESP_RETURN_ON_FALSE(s_sock >= 0, ESP_FAIL, TAG, "socket: errno %d", errno);
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_ANY), .sin_port = htons(VIDEO_PORT) };
    ESP_RETURN_ON_FALSE(bind(s_sock, (struct sockaddr *)&addr, sizeof(addr)) == 0, ESP_FAIL, TAG, "bind %d: errno %d", VIDEO_PORT, errno);
    /* The timeout IS the subscription clock: without it recvfrom blocks until the next
       datagram, and a viewer that simply stops sending is never seen to expire. */
    struct timeval tv = { .tv_sec = 0, .tv_usec = CTL_TICK_MS * 1000 };
    if (setsockopt(s_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        ESP_LOGE(TAG, "SO_RCVTIMEO: errno %d", errno);
        close(s_sock);
        s_sock = -1;
        return ESP_FAIL;
    }

    ESP_RETURN_ON_FALSE(xTaskCreate(sender_task, "video_tx", 4096, NULL, 3, &s_sender) == pdPASS, ESP_FAIL, TAG, "sender task");
    ESP_RETURN_ON_FALSE(xTaskCreate(enc_task, "video_enc", 6144, NULL, 3, NULL) == pdPASS, ESP_FAIL, TAG, "encoder task");
    ESP_RETURN_ON_FALSE(xTaskCreate(ctl_task, "video_ctl", 4096, NULL, 3, NULL) == pdPASS, ESP_FAIL, TAG, "control task");

    const esp_timer_create_args_t timer_args = { .callback = sender_wake, .name = "video_pace" };
    esp_timer_handle_t timer;
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &timer), TAG, "pace timer");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(timer, SEND_PERIOD_US), TAG, "pace timer start");

    ESP_LOGI(TAG, "video channel on UDP %d (%dx%d @%d, chunks of %d)", VIDEO_PORT, VIDEO_WIDTH, VIDEO_HEIGHT, VIDEO_FPS, VIDEO_CHUNK_BYTES);
    return ESP_OK;
}
