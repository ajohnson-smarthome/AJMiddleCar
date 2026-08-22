#ifndef RECOVERY_H
#define RECOVERY_H

#include <stdint.h>
#include <stdbool.h>

// Configurable history-window bounds (milliseconds).
#define RECOVER_WIN_MIN_MS 1000
#define RECOVER_WIN_MAX_MS 10000

// Load NVS config (enabled + window, defaults: ON, 5000 ms) and start the retreat
// task. Call once, BEFORE rt_link_start() — the control watchdog trips into it.
void recovery_init(void);

// Record one control frame into the breadcrumb buffer (call from rt_link on each frame
// the actuator actually took). Also bumps the liveness sequence.
void recovery_note_command(float t, float y);

// Called by rt_link's control watchdog when the link goes stale, INSTEAD of car_stop(). Decides:
// disabled / empty / stationary history → car_stop(); else → trigger the reverse replay.
void recovery_on_link_lost(void);

// Throw the breadcrumbs away: there is no path behind the car any more. Called by
// rt_link when a session ends (a goodbye) and when one begins (a hello adopted), which
// is what actually suppresses the retreat in both cases — an empty history has no
// motion in it, so a later trip degrades to a plain stop instead of retracing somebody
// else's drive. Also bumps the liveness sequence, so a replay already running aborts at
// its next step rather than finishing a path that no longer exists.
void recovery_forget(void);

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

// Pure (host-tested): one replay segment's duration, from the gap between two
// breadcrumb timestamps, capped at RECOVER_SEG_MAX_MS. Rollover-safe like the rest.
#define RECOVER_SEG_MAX_MS 250u
static inline uint32_t recovery_seg_ms(uint32_t newer_ts, uint32_t older_ts) {
    uint32_t d = newer_ts - older_ts;
    return d > RECOVER_SEG_MAX_MS ? RECOVER_SEG_MAX_MS : d;
}

#endif // RECOVERY_H
