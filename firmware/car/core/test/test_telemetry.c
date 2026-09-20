#define TELEMETRY_HOST_TEST
#include "../main/telemetry.h"
#include "contract.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>

int main(void) {
    char buf[RT_MAX_DATAGRAM];
    telemetry_t t = { .seq = 88, .rssi = -55, .rx_hz = 10, .timeouts = 2, .uptime_s = 123,
                      .free_heap = 198000, .calibrated = true, .owner = MOTORS_OWNER_REMOTE,
                      .bus_ok = true, .video_state = VIDEO_STATE_IDLE, .video_fps = 0,
                      .video_kbps = 0, .video_dropped = 0, .battery_state = BATTERY_STATE_OK,
                      .battery_mv = 12310, .battery_ma = 3100, .battery_mw = 38200,
                      .battery_soc = 72 };
    int n = telemetry_groups(buf, sizeof(buf), &t);
    assert(n > 0);
    assert(strcmp(buf,
        "\"link\":{\"rx_hz\":10,\"rssi_dbm\":-55,\"timeouts\":2},"
        "\"motors\":{\"bus\":\"ok\",\"calibrated\":true,\"owner\":\"remote\"},"
        "\"system\":{\"uptime_s\":123,\"free_heap\":198000}"
        ",\"video\":{\"state\":\"idle\",\"fps\":0,\"kbps\":0,\"dropped\":0}"
        ",\"battery\":{\"voltage_mv\":12310,\"current_ma\":3100,\"power_mw\":38200,"
            "\"soc_pct\":72,\"state\":\"ok\"}") == 0);

    n = telemetry_datagram(buf, sizeof(buf), &t);
    assert(n > 0 && n == (int)strlen(buf));
    assert(strcmp(buf,
        "{\"proto\":2,\"type\":\"telemetry\",\"seq\":88,"
        "\"link\":{\"rx_hz\":10,\"rssi_dbm\":-55,\"timeouts\":2},"
        "\"motors\":{\"bus\":\"ok\",\"calibrated\":true,\"owner\":\"remote\"},"
        "\"system\":{\"uptime_s\":123,\"free_heap\":198000}"
        ",\"video\":{\"state\":\"idle\",\"fps\":0,\"kbps\":0,\"dropped\":0}"
        ",\"battery\":{\"voltage_mv\":12310,\"current_ma\":3100,\"power_mw\":38200,"
            "\"soc_pct\":72,\"state\":\"ok\"}}") == 0);

    /* The push is a datagram, so the whole frame has to be one. Worst case: every
       counter wide, a negative RSSI, the longest owner name, and every battery number
       at the widest an int32 prints (a sign and ten digits — the printer's, not the
       pack's: the schema does not bound them). */
    telemetry_t wide = { .seq = 4294967295u, .rssi = -100, .rx_hz = 999,
                         .timeouts = 4294967295u, .uptime_s = 999999999,
                         .free_heap = 4294967295u, .calibrated = true,
                         .owner = MOTORS_OWNER_CALIBRATION, .bus_ok = false,
                         .video_state = VIDEO_STATE_STREAMING, .video_fps = 999,
                         .video_kbps = 4294967295u, .video_dropped = 4294967295u,
                         .battery_state = BATTERY_STATE_ABSENT, .battery_mv = -2147483647,
                         .battery_ma = -2147483647, .battery_mw = -2147483647,
                         .battery_soc = -2147483647 };
    int w = telemetry_datagram(buf, sizeof(buf), &wide);
    assert(w > 0);
    assert(w <= RT_MAX_DATAGRAM);
    assert(w > RT_MAX_COMMAND);   /* ...and would not fit the command cap */
    assert(strstr(buf, "\"bus\":\"down\""));
    printf("test_telemetry: widest push frame is %d bytes of %d\n", w, RT_MAX_DATAGRAM);

    /* A NULL owner is reported as idle rather than crashing snprintf. */
    t.owner = NULL;
    n = telemetry_groups(buf, sizeof(buf), &t);
    assert(n > 0 && strstr(buf, "\"owner\":\"idle\""));
    t.owner = MOTORS_OWNER_REMOTE;

    /* 0 dBm is "not measured", and the wire says so with null, not with a number that
       would read as a very strong signal. */
    t.calibrated = false; t.rssi = 0;
    n = telemetry_groups(buf, sizeof(buf), &t);
    assert(n > 0 && strstr(buf, "\"calibrated\":false") && strstr(buf, "\"rssi_dbm\":null"));

    /* No monitor: the word is absent and every number of the group is null — INT32_MIN
       is the sentinel, so a genuine zero (a pack at rest reads 0 mA) still prints as 0. */
    t.battery_state = BATTERY_STATE_ABSENT;
    t.battery_mv = t.battery_ma = t.battery_mw = t.battery_soc = INT32_MIN;
    n = telemetry_groups(buf, sizeof(buf), &t);
    assert(n > 0 && strstr(buf, ",\"battery\":{\"voltage_mv\":null,\"current_ma\":null,"
                                "\"power_mw\":null,\"soc_pct\":null,\"state\":\"absent\"}"));
    t.battery_state = BATTERY_STATE_OK; t.battery_ma = 0; t.battery_soc = 0;
    n = telemetry_groups(buf, sizeof(buf), &t);
    assert(n > 0 && strstr(buf, "\"current_ma\":0,") && strstr(buf, "\"soc_pct\":0,"));
    /* A NULL state word reads absent, the way a NULL owner reads idle. */
    t.battery_state = NULL;
    n = telemetry_groups(buf, sizeof(buf), &t);
    assert(n > 0 && strstr(buf, "\"state\":\"absent\"}"));

    assert(telemetry_groups(buf, 8, &t) == -1);
    assert(telemetry_datagram(buf, 40, &t) == -1);

    printf("test_telemetry: all passed\n");
    return 0;
}
