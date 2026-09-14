#include "../main/status_json.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static status_view_t sample(void) {
    status_view_t v = {
        .fw = "v1.0+789", .idf = "v6.0.2", .rolled_back = false,
        .usb_state = DONGLE_USB_STATE_UP,
        .ssid = "AJMiddleCar", .configured = true, .wifi_state = DONGLE_WIFI_STATE_CONNECTED,
        .connected = true, .rssi_dbm = -53, .channel = 1,
        .attempts_used = 0, .attempts_max = 5,
        .to_car_x10 = 100, .to_phone_x10 = 50, .udp_sessions = 1, .tcp_connections = 2,
        .video_sessions = 1, .video_kbps_x10 = 14872, .video_dropped = 3,
        .last_errno = 118, .last_error_message = "No route to host", .last_error_count = 3,
        .last_error_age_s = 41,
        .uptime_s = 412, .free_heap = 8551152,
    };
    return v;
}

int main(void) {
    char buf[640];
    status_view_t v = sample();
    int n = status_json_render(&v, buf, sizeof(buf));
    assert(n > 0 && n == (int)strlen(buf));
    assert(strcmp(buf,
        "{\"proto\":1,"
        "\"device\":{\"id\":\"ajdongle\",\"fw\":\"v1.0+789\",\"build\":789,\"rolled_back\":false,\"idf\":\"v6.0.2\"},"
        "\"usb\":{\"state\":\"up\"},"
        "\"wifi\":{\"ssid\":\"AJMiddleCar\",\"configured\":true,\"state\":\"connected\","
                  "\"rssi_dbm\":-53,\"channel\":1,\"attempts\":{\"used\":0,\"max\":5}},"
        "\"relay\":{\"to_car_hz\":10.0,\"to_phone_hz\":5.0,\"udp_sessions\":1,\"tcp_connections\":2,"
                   "\"last_error\":{\"errno\":118,\"message\":\"No route to host\",\"count\":3,\"age_s\":41},"
                   "\"video_sessions\":1,\"video_kbps\":1487.2,\"video_dropped\":3},"
        "\"system\":{\"uptime_s\":412,\"free_heap\":8551152}}") == 0);

    /* Not connected: the readings that need a link are null, not 0. Never failed: no
       error object at all. Nothing sent: an empty ssid and configured false. */
    v.connected = false; v.wifi_state = DONGLE_WIFI_STATE_IDLE; v.ssid = ""; v.configured = false;
    v.last_errno = 0; v.to_car_x10 = 7; v.to_phone_x10 = 0;
    v.video_sessions = 0; v.video_kbps_x10 = 0;
    n = status_json_render(&v, buf, sizeof(buf));
    assert(n > 0);
    assert(strstr(buf, "\"ssid\":\"\",\"configured\":false,\"state\":\"idle\",\"rssi_dbm\":null,\"channel\":null"));
    assert(strstr(buf, "\"last_error\":null"));
    assert(strstr(buf, "\"to_car_hz\":0.7,\"to_phone_hz\":0.0"));
    assert(strstr(buf, "\"video_sessions\":0,\"video_kbps\":0.0,\"video_dropped\":3"));

    /* A quote in the SSID is escaped, so the document stays one document. */
    v.ssid = "Say \"hi\"";
    n = status_json_render(&v, buf, sizeof(buf));
    assert(n > 0 && strstr(buf, "\"ssid\":\"Say \\\"hi\\\"\""));

    /* rolled_back and a version without a build number */
    v.rolled_back = true; v.fw = "v1.0";
    n = status_json_render(&v, buf, sizeof(buf));
    assert(n > 0 && strstr(buf, "\"build\":-1,\"rolled_back\":true"));

    /* Worst case fits: 32 quote bytes in the SSID, every counter wide. */
    char wide_ssid[33]; memset(wide_ssid, '"', 32); wide_ssid[32] = '\0';
    v = sample(); v.ssid = wide_ssid; v.rssi_dbm = -128; v.channel = 14;
    v.to_car_x10 = 65535; v.to_phone_x10 = 65535; v.udp_sessions = 4; v.tcp_connections = 4;
    v.last_errno = 2147483647; v.last_error_message = "Software caused connection abort";
    v.last_error_count = 4294967295u; v.last_error_age_s = 4294967295u;
    v.uptime_s = 2147483647L; v.free_heap = 4294967295u;
    v.video_kbps_x10 = 65535; v.video_dropped = 4294967295u;
    n = status_json_render(&v, buf, sizeof(buf));
    assert(n > 0 && n < 630);
    printf("test_status_json: widest body is %d bytes\n", n);

    assert(status_json_render(&v, buf, 64) == -1);
    printf("test_status_json: all passed\n");
    return 0;
}
