#ifndef RELAY_STATS_H
#define RELAY_STATS_H

#include <stdbool.h>
#include <stdint.h>

/* What the relays moved, counted without being read.
 *
 * Pure: no ESP-IDF, no clock of its own — the caller passes now_ms — so the arithmetic is
 * host-tested rather than reasoned about against a running device.
 *
 * The unit is PACKETS per second, never hertz. The 10 Hz control cadence belongs to
 * contract/car-api.json and is the app's and the car's; counting datagrams is this dongle's own
 * observation, and calling them a control frequency would borrow a meaning relay_udp.h
 * explicitly forbids it to know ("it only moves them"). Kept as tenths so the display can show
 * one decimal without floating point.
 *
 * Why this exists at all: on 2026-08-31 the dongle was joined, addressed and reading -27 dBm
 * while every relayed datagram failed with errno 12. Nothing in the system could say so. */
typedef struct {
    uint32_t total_to_car;      /* running totals, monotonic */
    uint32_t total_to_phone;
    uint32_t mark_to_car;       /* the totals as of the last sample */
    uint32_t mark_to_phone;
    uint32_t mark_ms;
    uint16_t to_car_x10;        /* packets per second x10, latched at the last sample */
    uint16_t to_phone_x10;
    int      last_errno;        /* 0 when nothing has failed */
    uint32_t errno_count;       /* repeats of last_errno, restarted when it changes */
    uint8_t  udp_used, udp_max;
    uint8_t  tcp_used, tcp_max;
} relay_stats_t;

void relay_stats_init(relay_stats_t *s, uint8_t udp_max, uint8_t tcp_max);

/* One datagram forwarded. `to_car` false means the other direction. */
void relay_stats_forwarded(relay_stats_t *s, bool to_car);

/* One forwarding failure, with its errno. */
void relay_stats_failed(relay_stats_t *s, int err);

/* One per relay, not one taking both: the two run in different tasks, and a single setter
 * would make each of them read the other's field and write it back — a lost update every time
 * the two passes interleave. */
void relay_stats_udp_slots(relay_stats_t *s, uint8_t used);
void relay_stats_tcp_slots(relay_stats_t *s, uint8_t used);

/* Close the window that began at the previous call and latch both rates. A window of zero
 * length leaves the previous reading in place rather than dividing by zero. */
void relay_stats_sample(relay_stats_t *s, uint32_t now_ms);

/* The one instance both relay tasks write and the display reads.
 *
 * Deliberately not a lock: every field is a single word written by one task and read by
 * another, and a reader that catches a torn pair sees a rate one sample stale — which is
 * cheaper than a mutex on the forwarding path, and the forwarding path is the one thing in
 * this firmware that must never wait. The two relays write disjoint fields except the errno
 * pair, where a lost update costs one repeat in a counter nobody adds up. */
relay_stats_t *relay_stats_shared(void);

#endif /* RELAY_STATS_H */
