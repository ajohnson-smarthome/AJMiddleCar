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

// Live telemetry snapshot: the five groups every push and every /status carries.
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
    uint32_t video_fps;        // frames sent in the last second (counted when a frame's last
                               // chunk leaves the sender, not at the encoder)
    uint32_t video_kbps;       // kbit sent in the last second
    uint32_t video_dropped;    // frames not sent since boot
    const char *battery_state; // one of the BATTERY_STATE_* words; NULL reads absent
    // The pack's numbers, TELEMETRY_NULL when there is nothing to report — the monitor is
    // absent, or the remainder is not yet determined. The wire's sign is the value's: a
    // charging pack is a negative battery_ma, and 0 is a genuine zero, not "no data".
    int32_t  battery_mv;       // pack voltage, mV
    int32_t  battery_ma;       // pack current, mA, discharge positive
    int32_t  battery_mw;       // power, mW, as the monitor computes it
    int32_t  battery_soc;      // remaining charge, 0..100
} telemetry_t;

// The "no value" of a nullable int32 field — printed as JSON null. INT32_MIN rather than
// 0 or -1: every one of the pack's numbers can legitimately be zero, and current can be
// negative.
#define TELEMETRY_NULL INT32_MIN

// A nullable int32 as the wire spells it: the number, or `null`. `buf` must hold 12.
static inline const char *telemetry_opt(char *buf, size_t n, int32_t v) {
    if (v == TELEMETRY_NULL) return "null";
    snprintf(buf, n, "%ld", (long)v);
    return buf;
}

// Pure: the "link", "motors", "system", "video" and "battery" members (NO surrounding
// braces, no trailing comma), in the schema's telemetry order — battery last, so /status
// can splice its own two groups in before "system" and keep the tail whole. Shared by the
// real-time push and /status. Every key is a generated macro, so a rename in the schema
// cannot survive here; test_contract_wire checks the nesting. Returns the length, or -1
// on truncation.
static inline int telemetry_groups(char *buf, size_t n, const telemetry_t *t) {
    char rssi[12], mv[12], ma[12], mw[12], soc[12];
    if (t->rssi != 0) snprintf(rssi, sizeof(rssi), "%d", t->rssi);
    else              snprintf(rssi, sizeof(rssi), "null");
    int r = snprintf(buf, n,
        "\"" KEY_GROUP_LINK "\":{\"" KEY_LINK_RX_HZ "\":%d,\"" KEY_LINK_RSSI_DBM "\":%s,"
            "\"" KEY_LINK_TIMEOUTS "\":%u},"
        "\"" KEY_GROUP_MOTORS "\":{\"" KEY_MOTORS_BUS "\":\"%s\",\"" KEY_MOTORS_CALIBRATED "\":%s,"
            "\"" KEY_MOTORS_OWNER "\":\"%s\"},"
        "\"" KEY_GROUP_SYSTEM "\":{\"" KEY_SYSTEM_UPTIME_S "\":%ld,\"" KEY_SYSTEM_FREE_HEAP "\":%u},"
        "\"" KEY_GROUP_VIDEO "\":{\"" KEY_VIDEO_STATE "\":\"%s\",\"" KEY_VIDEO_FPS "\":%u,"
            "\"" KEY_VIDEO_KBPS "\":%u,\"" KEY_VIDEO_DROPPED "\":%u},"
        "\"" KEY_GROUP_BATTERY "\":{\"" KEY_BATTERY_VOLTAGE_MV "\":%s,\"" KEY_BATTERY_CURRENT_MA "\":%s,"
            "\"" KEY_BATTERY_POWER_MW "\":%s,\"" KEY_BATTERY_SOC_PCT "\":%s,"
            "\"" KEY_BATTERY_STATE "\":\"%s\"}",
        t->rx_hz, rssi, (unsigned)t->timeouts,
        t->bus_ok ? MOTORS_BUS_OK : MOTORS_BUS_DOWN, t->calibrated ? "true" : "false",
        t->owner ? t->owner : MOTORS_OWNER_IDLE,
        t->uptime_s, (unsigned)t->free_heap,
        t->video_state ? t->video_state : VIDEO_STATE_OFF, (unsigned)t->video_fps,
        (unsigned)t->video_kbps, (unsigned)t->video_dropped,
        telemetry_opt(mv, sizeof(mv), t->battery_mv), telemetry_opt(ma, sizeof(ma), t->battery_ma),
        telemetry_opt(mw, sizeof(mw), t->battery_mw), telemetry_opt(soc, sizeof(soc), t->battery_soc),
        t->battery_state ? t->battery_state : BATTERY_STATE_ABSENT);
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
