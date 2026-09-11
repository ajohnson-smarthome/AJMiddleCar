#ifndef WATCHDOG_H
#define WATCHDOG_H

#include <stdint.h>
#include <stdbool.h>

// All that is left of the control watchdog: the one line of arithmetic that decides
// whether the driver has gone quiet. The state and the decision live in rt_link.c,
// because the task that notices silence should be the task that owns the channel — the
// old software timer ran at priority 1 and held the car's safety from there.

// Pure: has more than timeout_ms elapsed between last_ms and now_ms?
// Uses unsigned subtraction so 32-bit millisecond-counter rollover is handled.
static inline bool watchdog_stale(uint32_t last_ms, uint32_t now_ms, uint32_t timeout_ms) {
    return (uint32_t)(now_ms - last_ms) > timeout_ms;
}

#endif // WATCHDOG_H
