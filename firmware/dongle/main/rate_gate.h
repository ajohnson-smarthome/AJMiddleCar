#ifndef RATE_GATE_H
#define RATE_GATE_H

#include <stdbool.h>
#include <stdint.h>

/* Admission for the video relay: bytes per fixed window, and nothing else. Below the
 * limit every datagram passes; above it, the datagram is refused and the caller drops it
 * — on purpose, here, where it can be counted, rather than silently in esp_tinyusb when
 * the NTB pool is full, where a refused datagram might as easily have been the telemetry.
 * Pure, host-tested: the numbers come from contract/dongle-api.json. */
typedef struct {
    uint32_t limit_kbps;
    uint32_t window_ms;
    uint32_t window_start_ms;
    uint32_t bytes;            /* admitted in the current window */
} rate_gate_t;

void rate_gate_init(rate_gate_t *g, uint32_t limit_kbps, uint32_t window_ms);
/* True: the caller may send `bytes` now, and they are charged to the window. */
bool rate_gate_admit(rate_gate_t *g, uint32_t now_ms, uint32_t bytes);

#endif /* RATE_GATE_H */
