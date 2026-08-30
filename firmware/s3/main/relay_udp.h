#ifndef RELAY_UDP_H
#define RELAY_UDP_H

#include "esp_err.h"

/* The real-time channel (UDP, DONGLE_RELAY_RT_PORT), carried between the phone and the car a
 * byte at a time. See docs/superpowers/specs/2026-08-30-dongle-api-design.md, "The relay" — the
 * NAPT path was checked against lwIP's source and does not work for this topology, so the
 * dongle opens its own socket toward the car for every distinct phone and pumps bytes between
 * the two. It never parses a control frame or a telemetry push; it only moves them.
 *
 * Needs no guard of its own: the phone-facing socket binds to DONGLE_HOST specifically, not
 * INADDR_ANY, which is what keeps it from ever being reachable from the car's side of the
 * dongle — unlike the HTTP server, which the car's network can reach unless something stops
 * it. */

/* Starts the relay task. Safe to call right after wifi_sta_start(): the task waits on its own
 * for wifi_sta_gateway() to succeed before opening any socket, so this does not need to wait
 * for a join to finish and never blocks its caller. */
esp_err_t relay_udp_start(void);

#endif /* RELAY_UDP_H */
