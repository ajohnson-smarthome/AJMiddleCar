#ifndef TELEMETRY_H
#define TELEMETRY_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

// Live telemetry snapshot (changing fields only; device/fw stay in /status bootstrap).
typedef struct {
    int      rssi;        // dBm, 0 = no data
    int      ws_fps;      // control frames/sec
    uint32_t wdt_trips;   // watchdog auto-stops since boot
    long     uptime_s;    // seconds
    uint32_t heap;        // free heap, bytes
    bool     calibrated;  // valid calibration present
} telemetry_t;

// Pure: format the live fields (NO surrounding braces) into buf. Returns length, or -1 on truncation.
// Shared by the WS push ("{<fields>}") and /status ("{\"device\":..,\"fw\":..,<fields>}").
static inline int telemetry_fields(char *buf, size_t n, const telemetry_t *t) {
    int r = snprintf(buf, n,
        "\"rssi\":%d,\"ws_fps\":%d,\"wdt_trips\":%u,\"uptime_s\":%ld,\"heap\":%u,\"calibrated\":%s",
        t->rssi, t->ws_fps, (unsigned)t->wdt_trips, t->uptime_s, (unsigned)t->heap,
        t->calibrated ? "true" : "false");
    if (r < 0 || r >= (int)n) return -1;
    return r;
}

#ifndef TELEMETRY_HOST_TEST
#include "esp_err.h"
void      telemetry_gather(telemetry_t *out);  // read live values (IDF)
int       telemetry_json(char *buf, size_t n); // gather + "{<fields>}" for the WS push
esp_err_t telemetry_start(void);               // start the 5 Hz push timer
#endif

#endif // TELEMETRY_H
