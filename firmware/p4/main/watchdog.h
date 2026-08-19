#ifndef WATCHDOG_H
#define WATCHDOG_H

#include <stdint.h>
#include <stdbool.h>

// Start the control-link watchdog: a periodic check that stops the car if no
// watchdog_feed() has happened within timeout_ms. Call once after the WS server.
void watchdog_init(uint32_t timeout_ms);

// Record a fresh control frame (call from the WS handler on each valid frame).
// Also "arms" the watchdog so it only acts once traffic has started.
void watchdog_feed(void);

// How many times the watchdog auto-stopped the car since boot.
uint32_t watchdog_trips(void);

// Pure: has more than timeout_ms elapsed between last_ms and now_ms?
// Uses unsigned subtraction so 32-bit millisecond-counter rollover is handled.
static inline bool watchdog_stale(uint32_t last_ms, uint32_t now_ms, uint32_t timeout_ms) {
    return (uint32_t)(now_ms - last_ms) > timeout_ms;
}

#endif // WATCHDOG_H
