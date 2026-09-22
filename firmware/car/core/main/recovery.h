#ifndef RECOVERY_H
#define RECOVERY_H

#include <stdint.h>
#include <stdbool.h>
#ifdef ESP_PLATFORM
#  include "esp_err.h"
#else
   typedef int esp_err_t;
#  define ESP_OK 0
#endif

// Configurable history-window bounds (milliseconds) — contract.config.domains.recovery's
// window_ms range, spelled here because this header is pure and includes nothing;
// test_recovery pins the two together. The ceiling is not free: a retrace over the whole
// window starts RT_WATCHDOG_MS after the last accepted command and lasts one capped tail
// plus the window, while the session dies RT_SESSION_IDLE_MS after that same command and
// throws the path away with it. Session mortality is the senior budget, so the ceiling
// stays under it with room for ticks — at 10 000 the session's death cut the oldest
// half-second of the path instead of the retrace finishing it (AJM-129).
#define RECOVER_WIN_MIN_MS 1000
#define RECOVER_WIN_MAX_MS 8000

// Load NVS config (enabled + window, defaults: ON, 5000 ms) and start the retreat
// task. Call once, BEFORE rt_link_start() — the control watchdog trips into it.
void recovery_init(void);

// Record one control frame into the breadcrumb buffer (call from rt_link on each frame
// recovery_is_breadcrumb admits: the actuator took it and the bus was up). Also bumps
// the liveness sequence.
void recovery_note_command(float t, float y);

// Called by rt_link's control watchdog when the link goes stale, INSTEAD of car_stop(). Decides:
// disabled / bus down / empty / stationary history → car_stop(); else → trigger the
// reverse replay.
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
// Persist the current enabled+window config as a JSON string in NVS, and say
// whether it landed.
esp_err_t recovery_save(void);

// Pure (host-tested): a window held by the car — what recovery_set_config keeps and what a
// stored value becomes at boot. Clamped to the contract's range, not reset to the default:
// the ceiling came down from 10 000 to 8 000 (AJM-129), and a car whose owner had chosen
// the old ceiling boots with the new one, not with 5 000.
static inline uint16_t recovery_window_clamp(int window_ms) {
    if (window_ms < RECOVER_WIN_MIN_MS) return RECOVER_WIN_MIN_MS;
    if (window_ms > RECOVER_WIN_MAX_MS) return RECOVER_WIN_MAX_MS;
    return (uint16_t)window_ms;
}

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

// Pure (host-tested): does an accepted command become a breadcrumb? Only when the
// actuator took it AND the write would reach the wheels. The grant alone answers the
// arbiter's question, not the bus's: with the PWM boards down link.c swallows every
// write behind pca9685_ready(), and a path recorded then is a path the car never drove
// (AJM-169). Console commands never get here — they do not ride the real-time channel.
static inline bool recovery_is_breadcrumb(bool granted, bool bus_ok) {
    return granted && bus_ok;
}

// Pure (host-tested): may a lost link start a retrace at all? A retrace needs a live bus
// just as it needs a path: the boards can drop out mid-drive, after real motion was
// recorded, and retracing it on dead wheels would announce `recovering` for a car that
// stands still. Otherwise the lost link is a plain stop, the same as recovery off; an
// empty or motionless path is the retreat task's own verdict, further down.
static inline bool recovery_may_retrace(bool enabled, bool bus_ok) {
    return enabled && bus_ok;
}

#endif // RECOVERY_H
