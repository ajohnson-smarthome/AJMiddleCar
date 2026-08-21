#ifndef TELEMETRY_H
#define TELEMETRY_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

// Who is asking. The two consumers keep separate frame-rate accumulators: sharing them
// meant a /status poll consumed the push's measurement interval, so the number both
// reported was a function of how the callers interleaved rather than of the uplink.
typedef enum { TELEM_PUSH, TELEM_STATUS, TELEM_CONSUMERS } telem_consumer_t;

// Live telemetry snapshot (changing fields only; device/fw stay in the hello reply).
typedef struct {
    uint32_t seq;         // push counter, so the app can drop a reordered datagram
    int      rssi;        // dBm, 0 = no data
    int      rx_fps;      // control frames/sec
    uint32_t wdt_trips;   // watchdog auto-stops since boot
    long     uptime_s;    // seconds
    uint32_t heap;        // free heap, bytes
    bool     calibrated;  // valid calibration present
    const char *ctl;      // which source owns the actuator ("rt"/"recover"/"none"/…)
    bool     bus_ok;      // false once a PCA9685 write failed and has not since succeeded
} telemetry_t;

// Pure: format the live fields (NO surrounding braces) into buf. Returns length, or -1 on
// truncation. Shared by the real-time push ("{<fields>}") and /status
// ("{\"device\":..,\"fw\":..,<fields>}"). The names and their order are the schema's
// telemetry.fields; the C generator emits no symbols for them, so this is the one place
// they are spelled out.
static inline int telemetry_fields(char *buf, size_t n, const telemetry_t *t) {
    int r = snprintf(buf, n,
        "\"seq\":%u,\"rx_fps\":%d,\"rssi\":%d,\"wdt_trips\":%u,\"uptime_s\":%ld,\"heap\":%u,"
        "\"calibrated\":%s,\"bus_ok\":%s,\"ctl\":\"%s\"",
        (unsigned)t->seq, t->rx_fps, t->rssi, (unsigned)t->wdt_trips, t->uptime_s,
        (unsigned)t->heap,
        t->calibrated ? "true" : "false",
        t->bus_ok ? "true" : "false",
        t->ctl ? t->ctl : "none");
    if (r < 0 || r >= (int)n) return -1;
    return r;
}

#ifndef TELEMETRY_HOST_TEST
void telemetry_gather(telemetry_t *out, telem_consumer_t who);  // read live values (IDF)
int  telemetry_json(char *buf, size_t n);  // gather + "{<fields>}" for the rt_link push
#endif

#endif // TELEMETRY_H
