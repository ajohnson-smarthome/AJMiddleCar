#include "status_json.h"

#include <stdio.h>
#include <string.h>

#include "net_cfg.h"   /* net_cfg_escape: the one escaper, so /status cannot drift from /wifi */

/* The number after '+' in "v1.0+789", or -1. A deliberate twin of the car's
 * device_json.h helper: the two firmwares do not reference each other. */
static int build_number(const char *fw)
{
    const char *plus = strchr(fw, '+');
    if (plus == NULL || plus[1] < '0' || plus[1] > '9') {
        return -1;
    }
    int n = 0;
    for (const char *p = plus + 1; *p >= '0' && *p <= '9'; p++) {
        if (n > 214748363) {
            return -1;
        }
        n = n * 10 + (*p - '0');
    }
    return n;
}

int status_json_render(const status_view_t *v, char *buf, size_t n)
{
    char ssid_esc[72]; /* 32 SSID bytes, every one a quote, doubles to 64, +NUL */
    if (net_cfg_escape(v->ssid, ssid_esc, sizeof(ssid_esc)) < 0) {
        return -1;
    }
    char rssi[12], channel[12], last_error[128];
    if (v->connected) {
        snprintf(rssi, sizeof(rssi), "%d", v->rssi_dbm);
        snprintf(channel, sizeof(channel), "%u", v->channel);
    } else {
        snprintf(rssi, sizeof(rssi), "null");
        snprintf(channel, sizeof(channel), "null");
    }
    if (v->last_errno != 0) {
        int w = snprintf(last_error, sizeof(last_error),
                         "{\"" DONGLE_KEY_RELAY_LAST_ERROR_ERRNO "\":%d,"
                         "\"" DONGLE_KEY_RELAY_LAST_ERROR_MESSAGE "\":\"%s\","
                         "\"" DONGLE_KEY_RELAY_LAST_ERROR_COUNT "\":%u,"
                         "\"" DONGLE_KEY_RELAY_LAST_ERROR_AGE_S "\":%u}",
                         v->last_errno, v->last_error_message ? v->last_error_message : "",
                         v->last_error_count, v->last_error_age_s);
        if (w < 0 || (size_t)w >= sizeof(last_error)) {
            return -1;
        }
    } else {
        snprintf(last_error, sizeof(last_error), "null");
    }
    int r = snprintf(buf, n,
        "{\"" DONGLE_KEY_PROTO "\":%d,"
        "\"" DONGLE_KEY_GROUP_USB "\":{\"" DONGLE_KEY_USB_STATE "\":\"%s\"},"
        "\"" DONGLE_KEY_GROUP_WIFI "\":{"
            "\"" DONGLE_KEY_WIFI_SSID "\":\"%s\","
            "\"" DONGLE_KEY_WIFI_CONFIGURED "\":%s,"
            "\"" DONGLE_KEY_WIFI_STATE "\":\"%s\","
            "\"" DONGLE_KEY_WIFI_RSSI_DBM "\":%s,"
            "\"" DONGLE_KEY_WIFI_CHANNEL "\":%s,"
            "\"" DONGLE_KEY_WIFI_ATTEMPTS "\":{"
                "\"" DONGLE_KEY_WIFI_ATTEMPTS_USED "\":%u,"
                "\"" DONGLE_KEY_WIFI_ATTEMPTS_MAX "\":%u}},"
        "\"" DONGLE_KEY_GROUP_RELAY "\":{"
            "\"" DONGLE_KEY_RELAY_TO_CAR_HZ "\":%u.%u,"
            "\"" DONGLE_KEY_RELAY_TO_PHONE_HZ "\":%u.%u,"
            "\"" DONGLE_KEY_RELAY_UDP_SESSIONS "\":%u,"
            "\"" DONGLE_KEY_RELAY_TCP_CONNECTIONS "\":%u,"
            "\"" DONGLE_KEY_RELAY_LAST_ERROR "\":%s,"
            "\"" DONGLE_KEY_RELAY_VIDEO_SESSIONS "\":%u,"
            "\"" DONGLE_KEY_RELAY_VIDEO_KBPS "\":%u.%u,"
            "\"" DONGLE_KEY_RELAY_VIDEO_DROPPED "\":%u},"
        "\"" DONGLE_KEY_GROUP_SYSTEM "\":{"
            "\"" DONGLE_KEY_SYSTEM_UPTIME_S "\":%ld,"
            "\"" DONGLE_KEY_SYSTEM_FREE_HEAP "\":%u,"
            "\"" DONGLE_KEY_SYSTEM_IDF "\":\"%s\"}}",
        DONGLE_PROTO,
        v->usb_state,
        ssid_esc, v->configured ? "true" : "false", v->wifi_state, rssi, channel,
        v->attempts_used, v->attempts_max,
        v->to_car_x10 / 10u, v->to_car_x10 % 10u, v->to_phone_x10 / 10u, v->to_phone_x10 % 10u,
        v->udp_sessions, v->tcp_connections, last_error,
        v->video_sessions, v->video_kbps_x10 / 10u, v->video_kbps_x10 % 10u, v->video_dropped,
        v->uptime_s, v->free_heap, v->idf);
    if (r < 0 || (size_t)r >= n) {
        return -1;
    }
    return r;
}

int version_json_render(const char *fw, bool rolled_back, char *buf, size_t n)
{
    int r = snprintf(buf, n,
        "{\"" DONGLE_KEY_VERSION_DEVICE "\":\"" DONGLE_DEVICE "\","
        "\"" DONGLE_KEY_VERSION_FW "\":\"%s\","
        "\"" DONGLE_KEY_VERSION_BUILD "\":%d,"
        "\"" DONGLE_KEY_VERSION_PROTO "\":%d,"
        "\"" DONGLE_KEY_VERSION_ROLLED_BACK "\":%s}",
        fw, build_number(fw), DONGLE_PROTO, rolled_back ? "true" : "false");
    if (r < 0 || (size_t)r >= n) {
        return -1;
    }
    return r;
}
