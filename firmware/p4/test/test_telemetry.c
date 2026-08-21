#define TELEMETRY_HOST_TEST
#include "../main/telemetry.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>

int main(void) {
    char buf[224];
    telemetry_t t = { .seq = 88, .rssi = -55, .rx_fps = 10, .wdt_trips = 2,
                      .uptime_s = 123, .heap = 198000, .calibrated = true,
                      .ctl = "rt", .bus_ok = true };
    int n = telemetry_fields(buf, sizeof(buf), &t);
    assert(n > 0);
    assert(strcmp(buf,
        "\"seq\":88,\"rx_fps\":10,\"rssi\":-55,\"wdt_trips\":2,\"uptime_s\":123,\"heap\":198000,"
        "\"calibrated\":true,\"bus_ok\":true,\"ctl\":\"rt\"") == 0);

    /* The push is a datagram, so the whole frame has to be one. Worst case: every
       counter wide, a negative RSSI and the longest owner name. */
    telemetry_t wide = { .seq = 4294967295u, .rssi = -100, .rx_fps = 999,
                         .wdt_trips = 4294967295u, .uptime_s = 999999999,
                         .heap = 4294967295u, .calibrated = true, .ctl = "console",
                         .bus_ok = false };
    int w = telemetry_fields(buf, sizeof(buf), &wide);
    assert(w > 0);
    printf("test_telemetry: widest push frame is %d bytes\n", w + 2);  /* + the braces */

    // A NULL owner name is reported as "none" rather than crashing snprintf.
    t.ctl = NULL; t.bus_ok = false;
    n = telemetry_fields(buf, sizeof(buf), &t);
    assert(n > 0 && strstr(buf, "\"ctl\":\"none\"") && strstr(buf, "\"bus_ok\":false"));
    t.ctl = "rt"; t.bus_ok = true;

    t.calibrated = false; t.rssi = 0;
    n = telemetry_fields(buf, sizeof(buf), &t);
    assert(n > 0 && strstr(buf, "\"calibrated\":false") && strstr(buf, "\"rssi\":0"));

    assert(telemetry_fields(buf, 8, &t) == -1);

    printf("test_telemetry: all passed\n");
    return 0;
}
