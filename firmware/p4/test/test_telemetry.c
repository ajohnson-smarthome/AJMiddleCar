#define TELEMETRY_HOST_TEST
#include "../main/telemetry.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>

int main(void) {
    char buf[160];
    telemetry_t t = { .rssi = -55, .ws_fps = 10, .wdt_trips = 2,
                      .uptime_s = 123, .heap = 198000, .calibrated = true };
    int n = telemetry_fields(buf, sizeof(buf), &t);
    assert(n > 0);
    assert(strcmp(buf,
        "\"rssi\":-55,\"ws_fps\":10,\"wdt_trips\":2,\"uptime_s\":123,\"heap\":198000,\"calibrated\":true") == 0);

    t.calibrated = false; t.rssi = 0;
    n = telemetry_fields(buf, sizeof(buf), &t);
    assert(n > 0 && strstr(buf, "\"calibrated\":false") && strstr(buf, "\"rssi\":0"));

    assert(telemetry_fields(buf, 8, &t) == -1);

    printf("test_telemetry: all passed\n");
    return 0;
}
