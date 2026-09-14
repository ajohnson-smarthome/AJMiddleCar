#ifndef RELAY_STATS_H
#define RELAY_STATS_H

#include <stdatomic.h>
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
    uint32_t total_video_bytes;     /* toward the phone, on the video relay */
    uint32_t mark_video_bytes;
    uint16_t video_kbps_x10;        /* kbit/s x10, latched at the last sample */
    /* _Atomic, the three of them, and that is what makes their ORDER mean anything. Two
       readers test last_errno and then subtract from last_fail_ms, and relay_stats_failed
       stores the stamp first so the pair a reader sees is consistent — but as plain ints
       nothing held that order: the compiler may sink one independent store below another,
       or hoist a load, and the reader gets errno with a stamp of 0 and reports the device's
       whole uptime as the fault's age, on the first failure since boot, on the very frame
       it appears. Sequentially consistent atomics (C11's default for a plain access to an
       _Atomic object) pin the order on both sides. <stdatomic.h>, so the module stays
       host-testable with -std=c11. */
    _Atomic int      last_errno;    /* 0 when nothing has failed */
    _Atomic uint32_t errno_count;   /* repeats of last_errno, restarted when it changes */
    _Atomic uint32_t last_fail_ms;  /* when the last failure happened; 0 when none has */
    _Atomic uint32_t video_dropped; /* chunks the admission limit refused since boot */
    uint8_t  udp_used;
    uint8_t  tcp_used;
    uint8_t  video_used;
} relay_stats_t;

/* Takes no pool sizes. It carried udp_max and tcp_max until nothing turned out to read them:
 * the display prints «Слоты TCP 1 UDP 2» with no denominator, dongle_view_t has no field for
 * one, and /status emits only the used counts. Their one real cost was not the two bytes — it
 * was relay_udp.c including relay_tcp.h to pass RELAY_POOL_SIZE into a field nobody read, one
 * relay reaching into the other's header for a number neither of them used. */
void relay_stats_init(relay_stats_t *s);

/* One datagram forwarded. `to_car` false means the other direction. */
void relay_stats_forwarded(relay_stats_t *s, bool to_car);

/* One forwarding failure, with its errno and the moment it happened.
 *
 * The errno LATCHES — nothing clears it on a later success, deliberately: a fault that healed
 * is still a fault that happened, and a reader that has never seen one is entitled to know the
 * difference. That alone made this instrument answer its own founding question wrong, though.
 * A single failure at boot and a link failing continuously right now leave identical fields,
 * and the fault page and /status reported both as current. `last_fail_ms` is what separates
 * them: the record is kept AND its age is knowable, where clearing on success would have
 * thrown the record away and clearing on nothing at all kept a lie. The caller passes the
 * clock for the same reason relay_stats_sample does — this module has none of its own; every
 * caller passes boot_ms() (dongle_clock.h), the one clock this firmware keeps. */
void relay_stats_failed(relay_stats_t *s, int err, uint32_t now_ms);

/* One per relay, not one taking both: the two run in different tasks, and a single setter
 * would make each of them read the other's field and write it back — a lost update every time
 * the two passes interleave. */
void relay_stats_udp_slots(relay_stats_t *s, uint8_t used);
void relay_stats_tcp_slots(relay_stats_t *s, uint8_t used);

/* The video relay's own bookkeeping — see relay_udp.h's `video` flag. One forwarded chunk,
 * one refusal (rate_gate turned it away, so the caller never sent it and there is nothing
 * to attribute to `total_video_bytes`), and the live session count. */
void relay_stats_video_forwarded(relay_stats_t *s, uint32_t bytes);
void relay_stats_video_dropped(relay_stats_t *s);
void relay_stats_video_slots(relay_stats_t *s, uint8_t used);

/* Close the window that began at the previous call and latch both rates. A window of zero
 * length leaves the previous reading in place rather than dividing by zero. */
void relay_stats_sample(relay_stats_t *s, uint32_t now_ms);

/* The one instance both relay tasks write and the display reads.
 *
 * Deliberately not a lock: every field is a single word written by at most one task, except
 * the errno pair, and a reader that catches a torn pair sees a rate one sample stale — which
 * is cheaper than a mutex on the forwarding path, and the forwarding path is the one thing in
 * this firmware that must never wait. The packet totals belong to the UDP relay alone —
 * relay_tcp.c never calls relay_stats_forwarded(), on purpose: the display's «Пакеты 10/5 в
 * сек» reads as the real-time control rate only because every count in it is a control
 * datagram, and mixing in REST bytes moved by the byte-pumping TCP relay would make a burst
 * of REST fragments indistinguishable from a healthy control loop, not merely disagree with
 * this comment. The two relays' slot counts are likewise each their own field. Only the
 * errno pair is written by both — both relays speak to the same car over the same radio, so
 * whichever wrote it means the same thing — and there a lost update costs one repeat in a
 * counter nobody adds up. */
relay_stats_t *relay_stats_shared(void);

#endif /* RELAY_STATS_H */
