#ifndef RECOVERY_H
#define RECOVERY_H

#include <stdint.h>
#include <stdbool.h>

// Configurable history-window bounds (milliseconds).
#define RECOVER_WIN_MIN_MS 1000
#define RECOVER_WIN_MAX_MS 10000

// Load NVS config (enabled + window, defaults: ON, 5000 ms) and start the retreat
// task. Call once, BEFORE watchdog_init().
void recovery_init(void);

// Record one control frame into the breadcrumb buffer (call from the WS handler on
// each valid frame, alongside watchdog_feed). Also bumps the liveness sequence.
void recovery_note_command(float t, float y);

// Called by the watchdog when the link goes stale, INSTEAD of car_stop(). Decides:
// disabled / empty / stationary history → car_stop(); else → trigger the reverse replay.
void recovery_on_link_lost(void);

// Config getters/setters (RAM; the API layer persists to NVS).
void recovery_set_config(bool enabled, uint16_t window_ms);
void recovery_get_config(bool *enabled, uint16_t *window_ms);
// Persist the current enabled+window config as a JSON string in NVS.
void recovery_save(void);

// Pure (host-tested): reverse a command = negate both axes.
static inline void recovery_reverse(float t, float y, float *rt, float *ry) {
    *rt = -t;
    *ry = -y;
}

// Pure (host-tested): is a sample taken at `ts` older than `window_ms` before `now`?
// Unsigned subtraction → 32-bit millisecond-counter rollover is handled.
static inline bool recovery_evict(uint32_t ts, uint32_t now, uint16_t window_ms) {
    return (uint32_t)(now - ts) > window_ms;
}

#endif // RECOVERY_H
