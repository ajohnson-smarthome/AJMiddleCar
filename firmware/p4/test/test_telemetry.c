#define TELEMETRY_HOST_TEST
#include "../main/telemetry.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>

int main(void) {
    char buf[224];
    telemetry_t t = { .rssi = -55, .ws_fps = 10, .wdt_trips = 2,
                      .uptime_s = 123, .heap = 198000, .calibrated = true,
                      .ctl = "rt", .bus_ok = true };
    int n = telemetry_fields(buf, sizeof(buf), &t);
    assert(n > 0);
    assert(strcmp(buf,
        "\"rssi\":-55,\"ws_fps\":10,\"wdt_trips\":2,\"uptime_s\":123,\"heap\":198000,"
        "\"calibrated\":true,\"ctl\":\"rt\",\"bus_ok\":true") == 0);

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
