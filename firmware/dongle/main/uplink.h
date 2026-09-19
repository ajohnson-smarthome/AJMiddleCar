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
 * the car and nothing comes back. The score is TIME, not sends: a streak of silence begins
 * with the first send toward the car that got nothing back and lasts until the car is heard
 * from — a datagram on either relay, bytes on a TCP slot, a SYN answered. Counting sends was
 * the first version, twenty of them, and that number was really two seconds of control at
 * 10 Hz: on the launch ladder's REST polls every 3.5 s the same twenty took seventy seconds,
 * longer than the update's whole reboot window (AJM-124), while on a 10 Hz drive stream it
 * fired two seconds into the car's breadcrumb return, which is silence the car is ALLOWED —
 * and the kick's reassociation then cut the very return it was retracing (AJM-96). Measured
 * as time, the streak means the same thing whatever the phone happens to be sending.
 *
 * The threshold is sized against the longest silence a live car may keep: the breadcrumb
 * return runs for up to recovery.window_ms — 8 000 at the contract's ceiling
 * (contract/car-api.json, config.domains.recovery.window_ms.max) — after a control watchdog of
 * 300 ms, and a car that comes back into range at the end of it answers the next frame. Nine
 * seconds clears that with margin; the number is this firmware's own, not the contract's.
 * Silence is only evidence while the phone keeps asking, though: `bye` is answered by nothing
 * (docs/protocol.md), and a phone that sends one and goes to the background for a minute has
 * not been ignored for a minute. A pause in the SENDS as long as the threshold therefore starts
 * the measurement over — the streak is the span from its first send to its latest, and it only
 * grows while sends keep coming less than a threshold apart.
 *
 * A streak past the threshold is a dead uplink the station cannot see, and the caller answers
 * it with a reassociation, rate-limited so a radio that is already re-joining is not kicked
 * again mid-attempt: 10 s between kicks covers a full join.
 *
 * Stamps are _Atomic because three relay tasks score the same instance and none of them may
 * wait on the others. A race costs at most one send's worth of streak; the kick decision itself
 * is a compare-and-swap on the last-kick time, so a streak that ends on two tasks at once asks
 * the radio once, not twice. */
#define UPLINK_DEAD_AFTER_MS     9000u
#define UPLINK_KICK_SPACING_MS   10000u

typedef struct {
    _Atomic uint32_t silent_since_ms;  /* the streak's first unanswered send; 0 = no streak */
    _Atomic uint32_t streak_ms;        /* its length as of its latest send — one word, so a
                                          reader's verdict is never torn between two */
    _Atomic uint32_t last_kick_ms;     /* boot_ms of the last kick asked for; 0 = never */
} uplink_t;

void uplink_init(uplink_t *u);
/* Something arrived from the car: the uplink is alive, the streak is over. */
void uplink_heard(uplink_t *u);
/* A send toward the car went out (`sent`) or could not (`failed`) and, as of now_ms, nothing
 * has answered it — a datagram either way, or a connection attempt at the moment it is known
 * to be unanswered (the phone gave up on it, or the relay's own deadline did). True when the
 * caller should ask the station to reassociate NOW: the streak has run for UPLINK_DEAD_AFTER_MS
 * and no kick was asked for within the last UPLINK_KICK_SPACING_MS. Only the caller that gets
 * `true` acts on it, and the streak restarts with the kick, so the re-joined association has a
 * whole threshold in which to answer. Two names for one rule, so a call site reads as what it
 * observed. */
bool uplink_sent(uplink_t *u, uint32_t now_ms);
bool uplink_failed(uplink_t *u, uint32_t now_ms);
/* Whether the streak, as last measured, has reached UPLINK_DEAD_AFTER_MS — what net_api reads
 * to tell a connected station that is carrying traffic from one that only says it is. No clock:
 * a streak's length is send-to-send, so a phone that stopped asking leaves the last verdict
 * standing rather than ageing it into a fault. */
bool uplink_dead(const uplink_t *u);
/* The one instance every relay scores and net_api reads. */
uplink_t *uplink_shared(void);

#endif /* UPLINK_H */
