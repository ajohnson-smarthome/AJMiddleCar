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
                      .bus_ok = true };
    int n = telemetry_groups(buf, sizeof(buf), &t);
    assert(n > 0);
    assert(strcmp(buf,
        "\"link\":{\"rx_hz\":10,\"rssi_dbm\":-55,\"timeouts\":2},"
        "\"motors\":{\"bus\":\"ok\",\"calibrated\":true,\"owner\":\"remote\"},"
        "\"system\":{\"uptime_s\":123,\"free_heap\":198000}") == 0);

    n = telemetry_datagram(buf, sizeof(buf), &t);
    assert(n > 0 && n == (int)strlen(buf));
    assert(strcmp(buf,
        "{\"proto\":2,\"type\":\"telemetry\",\"seq\":88,"
        "\"link\":{\"rx_hz\":10,\"rssi_dbm\":-55,\"timeouts\":2},"
        "\"motors\":{\"bus\":\"ok\",\"calibrated\":true,\"owner\":\"remote\"},"
        "\"system\":{\"uptime_s\":123,\"free_heap\":198000}}") == 0);

    /* The push is a datagram, so the whole frame has to be one. Worst case: every
       counter wide, a negative RSSI and the longest owner name. */
    telemetry_t wide = { .seq = 4294967295u, .rssi = -100, .rx_hz = 999,
                         .timeouts = 4294967295u, .uptime_s = 999999999,
                         .free_heap = 4294967295u, .calibrated = true,
                         .owner = MOTORS_OWNER_CALIBRATION, .bus_ok = false };
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

    assert(telemetry_groups(buf, 8, &t) == -1);
    assert(telemetry_datagram(buf, 40, &t) == -1);

    printf("test_telemetry: all passed\n");
    return 0;
}
