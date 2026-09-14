#ifndef UPLINK_H
#define UPLINK_H

#include <stdbool.h>
#include <stdint.h>

/* Whether the path to the car is actually carrying anything — as opposed to what the station
 * says about its association, which is a different question with a different answer.
 *
 * Found on the bench 2026-09-14, twice in one evening. The car rebooted (an OTA), its softAP
 * came back and had forgotten the dongle, and the dongle never noticed: wifi.state stayed
 * `connected`, RSSI kept updating from the beacons it could still hear, and nothing it sent
 * arrived. The first time, with video streaming, the radio's transmit buffers filled with
 * frames nobody acknowledged and every send failed with ENOMEM; the second time, with only
 * hello at 5 Hz, the sends all SUCCEEDED — the frames simply vanished after their retries.
 * No disconnect event either time, so wifi_state's budget never ran, and POST /wifi with an
 * unchanged network is a no-op while the station reports connected. Only a replug recovered
 * it — and a car reboots on every update.
 *
 * So the relays keep score of the one thing that is true in both cases: traffic goes toward
 * the car and nothing comes back. Every send toward the car is a strike, whether it failed or
 * not; every datagram or byte FROM the car clears the count. A streak of strikes is a dead
 * uplink the station cannot see, and the caller answers it with a reassociation, rate-limited
 * so a radio that is already re-joining is not kicked again mid-attempt. Pure, host-tested;
 * the numbers are judgements: 20 unanswered sends is two seconds of a phone streaming control
 * at 10 Hz, or four of hello at 5 Hz — long enough that an ARP or a lost frame or two cannot
 * trip it, since a live car answers every hello and pushes telemetry five times a second;
 * 10 s between kicks covers a full join.
 *
 * Counters are _Atomic because three relay tasks score the same instance and none of them may
 * wait on the others. A race costs at most one miscounted strike; the kick decision itself is
 * a compare-and-swap on the last-kick time, so a streak that ends on two tasks at once asks the
 * radio once, not twice. */
#define UPLINK_DEAD_AFTER        20u
#define UPLINK_KICK_SPACING_MS   10000u

typedef struct {
    _Atomic uint32_t strikes;        /* sends toward the car since the last thing heard from it */
    _Atomic uint32_t last_kick_ms;   /* boot_ms of the last kick asked for; 0 = never */
} uplink_t;

void uplink_init(uplink_t *u);
/* Something arrived from the car: the uplink is alive, the streak is over. */
void uplink_heard(uplink_t *u);
/* A send toward the car went out (`sent`) or could not (`failed`) — one strike either way.
 * True when the caller should ask the station to reassociate NOW: the streak has reached
 * UPLINK_DEAD_AFTER and no kick was asked for within the last UPLINK_KICK_SPACING_MS. Only the
 * caller that gets `true` acts on it. Two names for one rule, so a call site reads as what it
 * observed. */
bool uplink_sent(uplink_t *u, uint32_t now_ms);
bool uplink_failed(uplink_t *u, uint32_t now_ms);
/* Whether the streak stands at UPLINK_DEAD_AFTER or beyond — what net_api reads to tell a
 * connected station that is carrying traffic from one that only says it is. */
bool uplink_dead(const uplink_t *u);
/* The one instance every relay scores and net_api reads. */
uplink_t *uplink_shared(void);

#endif /* UPLINK_H */
