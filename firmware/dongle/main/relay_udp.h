#ifndef RELAY_UDP_H
#define RELAY_UDP_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* One datagram relay: the phone's datagrams on `port` go to gateway:`port`, replies come
 * back to the phone that sent them. Run twice — the real-time channel and the video
 * channel — as two tasks over the same code, differing in port, priority and bookkeeping.
 * The video instance is admitted through rate_gate toward the phone (see rate_gate.h) and
 * reports on the relay.video_* fields; the control instance is never throttled.
 * `cfg` must outlive the task: a static const in main.c. */
typedef struct {
    uint16_t    port;
    const char *name;        /* the task name, and the log tag's suffix */
    unsigned    priority;
    bool        video;
} relay_udp_cfg_t;

esp_err_t relay_udp_start(const relay_udp_cfg_t *cfg);

#endif /* RELAY_UDP_H */
