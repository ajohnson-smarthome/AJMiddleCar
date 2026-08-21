#ifndef RT_LINK_H
#define RT_LINK_H

#include <stdint.h>
#include "esp_err.h"

// The real-time channel: one UDP socket, one task, one owner.
//
// The task's receive timeout is its tick, so the three things that used to live in
// three places happen in one loop — apply the owner's command, notice that the owner
// has gone quiet, and push telemetry back. That is why the control watchdog no longer
// has a file of its own: the task that notices silence is the task that owns the
// channel, rather than a priority-1 timer callback holding the car's safety.
//
// Call after wifi_ap_start() and recovery_init(); it needs neither the HTTP server nor
// the console.
esp_err_t rt_link_start(void);

// Control frames accepted since boot — telemetry's rx_fps is the derivative of this.
// Written only by the rt_link task, read by whoever gathers telemetry; an aligned u32
// load is atomic, and a reader that is one frame behind reports a rate, not a fact.
uint32_t rt_link_frames(void);

// How many times the control watchdog has declared the link lost since boot. An
// increment is the difference between a driver who said goodbye and one who walked out
// of range, so the app plots it rather than merely logging it.
uint32_t rt_link_wdt_trips(void);

#endif // RT_LINK_H
