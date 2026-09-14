#ifndef TELEMETRY_H
#define TELEMETRY_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include "contract.h"   /* the group/field keys and the MOTORS_OWNER_* vocabulary — the schema's, not ours */

// Who is asking. The two consumers keep separate frame-rate accumulators: sharing them
// meant a /status poll consumed the push's measurement interval, so the number both
// reported was a function of how the callers interleaved rather than of the uplink.
typedef enum { TELEM_PUSH, TELEM_STATUS, TELEM_CONSUMERS } telem_consumer_t;

// Live telemetry snapshot: the three groups every push and every /status carries.
typedef struct {
    uint32_t seq;         // push counter, so the app can drop a reordered datagram
    int      rssi;        // dBm, 0 = not measured (printed as null)
    int      rx_hz;       // drive datagrams/sec
    uint32_t timeouts;    // watchdog trips since boot
    long     uptime_s;    // seconds
    uint32_t free_heap;   // bytes
    bool     calibrated;  // valid calibration present
    bool     bus_ok;      // false once a PCA9685 write failed and has not since succeeded
    const char *owner;    // which source owns the actuator: one of the MOTORS_OWNER_* words
    const char *video_state;   // one of the VIDEO_STATE_* words
    uint32_t video_fps;        // frames encoded in the last second
    uint32_t video_kbps;       // kbit sent in the last second
    uint32_t video_dropped;    // frames not sent since boot
} telemetry_t;

// Pure: the "link", "motors", "system" and "video" members (NO surrounding braces, no
// trailing comma). Shared by the real-time push and /status. Every key is a generated
// macro, so a rename in the schema cannot survive here; test_contract_wire checks the
// nesting. Returns the length, or -1 on truncation.
static inline int telemetry_groups(char *buf, size_t n, const telemetry_t *t) {
    char rssi[12];
    if (t->rssi != 0) snprintf(rssi, sizeof(rssi), "%d", t->rssi);
    else              snprintf(rssi, sizeof(rssi), "null");
    int r = snprintf(buf, n,
        "\"" KEY_GROUP_LINK "\":{\"" KEY_LINK_RX_HZ "\":%d,\"" KEY_LINK_RSSI_DBM "\":%s,"
            "\"" KEY_LINK_TIMEOUTS "\":%u},"
        "\"" KEY_GROUP_MOTORS "\":{\"" KEY_MOTORS_BUS "\":\"%s\",\"" KEY_MOTORS_CALIBRATED "\":%s,"
            "\"" KEY_MOTORS_OWNER "\":\"%s\"},"
        "\"" KEY_GROUP_SYSTEM "\":{\"" KEY_SYSTEM_UPTIME_S "\":%ld,\"" KEY_SYSTEM_FREE_HEAP "\":%u},"
        "\"" KEY_GROUP_VIDEO "\":{\"" KEY_VIDEO_STATE "\":\"%s\",\"" KEY_VIDEO_FPS "\":%u,"
            "\"" KEY_VIDEO_KBPS "\":%u,\"" KEY_VIDEO_DROPPED "\":%u}",
        t->rx_hz, rssi, (unsigned)t->timeouts,
        t->bus_ok ? MOTORS_BUS_OK : MOTORS_BUS_DOWN, t->calibrated ? "true" : "false",
        t->owner ? t->owner : MOTORS_OWNER_IDLE,
        t->uptime_s, (unsigned)t->free_heap,
        t->video_state ? t->video_state : VIDEO_STATE_OFF, (unsigned)t->video_fps,
        (unsigned)t->video_kbps, (unsigned)t->video_dropped);
    if (r < 0 || r >= (int)n) return -1;
    return r;
}

// Pure: the whole push datagram, {"proto":2,"type":"telemetry","seq":N,<groups>}.
static inline int telemetry_datagram(char *buf, size_t n, const telemetry_t *t) {
    int r = snprintf(buf, n, "{\"" KEY_PROTO "\":%d,\"" RT_KEY_TYPE "\":\"" RT_TYPE_TELEMETRY "\","
                             "\"" RT_KEY_SEQ "\":%u,", RT_PROTO, (unsigned)t->seq);
    if (r < 0 || r >= (int)n) return -1;
    int g = telemetry_groups(buf + r, n - (size_t)r, t);
    if (g < 0) return -1;
    r += g;
    if ((size_t)r + 2 > n) return -1;   /* the brace and the NUL */
    buf[r++] = '}';
    buf[r] = '\0';
    return r;
}

#ifndef TELEMETRY_HOST_TEST
#include "esp_err.h"

// Start the 1 Hz RSSI sampler. Call after wifi_ap_start() and before rt_link_start();
// without it every gather reports rssi 0, which is its documented "no data" value.
// It is a task rather than a line in telemetry_gather because the AP-side RSSI is an
// SDIO RPC to the radio co-processor, and gathering happens on the task that holds the
// car's safety — see telemetry.c.
esp_err_t telemetry_start(void);

void telemetry_gather(telemetry_t *out, telem_consumer_t who);  // read live values (IDF)
int  telemetry_json(char *buf, size_t n);  // gather + telemetry_datagram for the rt_link push
#endif

#endif // TELEMETRY_H
