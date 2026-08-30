# Dongle Radio and Relay Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The dongle joins the car's network and carries the app's traffic to it, so the phone
reaches the car at `192.168.7.1` without leaving its own Wi-Fi.

**Architecture:** A station whose join is driven by a pure state machine, and two socket-layer
relays — TCP for the car's REST surface, UDP for the real-time channel — that interpret nothing
and forward to the gateway the join produced. The dongle's own API gains a guard so it answers
only on the USB wire.

**Tech Stack:** ESP-IDF 6.0.2, `esp_wifi` (the S3 has its own radio — no `esp_hosted`), BSD
sockets over lwIP, `esp_http_server`.

**Spec:** `docs/superpowers/specs/2026-08-30-dongle-api-design.md`

**Why a relay and not NAT.** The spec's "Why lwIP's NAPT cannot do it" section carries the
finding in full. In short: `ip_portmap_find` is pure DNAT and never touches the source, the
masquerade the car's reply depends on comes from `ip_napt_forward` on the output interface, and
`ip4.c` gates those two hooks on opposite states of one exclusive netif flag that also allocates
the tables. Neither placement works. Do not re-litigate this in an implementation task; if
something here contradicts it, the spec wins.

## Global Constraints

- **The dongle knows no car.** No SSID, password, device id, protocol or address may be compiled
  into `firmware/s3`. The relays' destination is **the gateway the join produced**, read at
  runtime from `IP_EVENT_STA_GOT_IP`. A hard-coded `192.168.4.1` anywhere is a defect.
- **The relays interpret nothing.** They move bytes between two sockets. No parsing, no
  buffering of whole messages, no knowledge of JSON or of the control frame.
- **The dongle's own API must not answer over the radio.** `POST /net` carries a Wi-Fi password
  and `POST /ota` writes firmware; both are USB-only today only because no second interface
  exists. This plan creates that interface, so this plan brings the guard. Nothing in it may be
  merged with the guard missing.
- **Generated files are never hand-edited.** `firmware/s3/main/dongle_contract.inc` and
  `app/AJMiddleCar/Generated/DongleAPI.swift` come from `contract/dongle-api.json` through
  `tools/gen_contract.py`.
- **Pure modules stay pure.** `wifi_state.{c,h}` and `udp_sess.{c,h}` include no ESP-IDF header
  and are host-tested by `firmware/s3/test/Makefile` with `cc -Wall -Wextra -Werror -std=c11`,
  exactly as `net_cfg` already is.
- **No real network credentials anywhere** — not in tests, not in scripts, not in docs.
- **ESP-IDF 6.0.2**, sourced with `tools/env-p4.sh`; the target comes from `sdkconfig.defaults`.
- **`tools/test-all.sh` must be green before every commit.**

---

## File Structure

| File | Responsibility |
|---|---|
| `contract/dongle-api.json` | *modify* — the relay's two forwarded ports become vocabulary |
| `tools/gen_dongle.py`, `tools/test_gen_contract.py` | *modify* — emit and assert them |
| `firmware/s3/main/wifi_state.{c,h}` | *create* — **pure**: the join state machine and its retry budget |
| `firmware/s3/main/wifi_sta.{c,h}` | *create* — the radio: events in, state machine driven, gateway and RSSI out |
| `firmware/s3/main/udp_sess.{c,h}` | *create* — **pure**: the real-time channel's session table |
| `firmware/s3/main/relay_udp.{c,h}` | *create* — one socket per phone session, both directions |
| `firmware/s3/main/relay_tcp.{c,h}` | *create* — a bounded pool of forwarded connections, one select loop |
| `firmware/s3/main/api_guard.{c,h}` | *create* — refuses a connection whose local address is not the USB one |
| `firmware/s3/main/status_api.c` | *modify* — `net.state` and `net.rssi` stop being constants; the guard is installed |
| `firmware/s3/main/net_api.c` | *modify* — a changed `POST /net` starts a join |
| `firmware/s3/main/main.c` | *modify* — start the radio and the relays |
| `firmware/s3/main/CMakeLists.txt`, `firmware/s3/sdkconfig.defaults` | *modify* — sources, `esp_wifi`, station tuning |
| `firmware/s3/test/Makefile`, `test_wifi_state.c`, `test_udp_sess.c` | *modify/create* — the two pure modules' host tests |
| `firmware/s3/README.md`, `firmware/s3/verify-on-host.sh` | *modify* — the bench record and what it runs |

---

### Task 1: The contract learns the relay's ports

The two ports the dongle forwards are part of what the app must agree with it: the app reaches
the car at `192.168.7.1:80` and `192.168.7.1:4210` because the dongle chose to listen there.
They belong in the schema like every other agreed number.

They are the car's native ports, and that is not a contradiction of "the dongle knows no car" —
the dongle knows which ports it forwards, not what speaks on them.

**Files:**
- Modify: `contract/dongle-api.json`, `tools/gen_dongle.py`, `tools/test_gen_contract.py`
- Generated (regenerate, never hand-edit): `firmware/s3/main/dongle_contract.inc`,
  `app/AJMiddleCar/Generated/DongleAPI.swift`

**Interfaces:**
- Produces: `DONGLE_RELAY_HTTP_PORT` (80) and `DONGLE_RELAY_RT_PORT` (4210) in C;
  `DongleContract.relayHttpPort` and `.relayRtPort` in Swift. Tasks 4 and 5 use them.

- [ ] **Step 1: Add the ports to the schema**

In `contract/dongle-api.json`, after the `network` object:

```json
  "relay": {
    "http_port": 80,
    "rt_port": 4210,
    "doc": "The ports the dongle listens on for the car's traffic, on its own address. The car keeps its native numbers so contract/car-api.json and CarHost.port do not move; only CarHost.host does. What speaks on them is not the dongle's business."
  },
```

- [ ] **Step 2: Write the failing assertions**

In `tools/test_gen_contract.py`, add to the existing `TestDongleEmitters` methods — the C ports
alongside the other C constants, the Swift ones alongside the other Swift constants. Follow the
file's idiom: these are `self.g.emit_dongle_c(self.s)` / `emit_dongle_swift(self.s)` assertions
inside the methods that already cover this vocabulary, not new test methods.

```python
        self.assertIn("#define DONGLE_RELAY_HTTP_PORT 80", out)
        self.assertIn("#define DONGLE_RELAY_RT_PORT 4210", out)
```
```python
        self.assertIn("public static let relayHttpPort: UInt16 = 80", out)
        self.assertIn("public static let relayRtPort: UInt16 = 4210", out)
```

Run `python3 tools/test_gen_contract.py`. Expected: the four new assertions fail — the schema has
the values but neither emitter names them.

- [ ] **Step 3: Emit them**

In `emit_dongle_c`, after the `DONGLE_PORT` line:

```python
        f"#define DONGLE_RELAY_HTTP_PORT {schema['relay']['http_port']}",
        f"#define DONGLE_RELAY_RT_PORT {schema['relay']['rt_port']}",
```

In `emit_dongle_swift`, after the `port` line:

```python
        f"    public static let relayHttpPort: UInt16 = {schema['relay']['http_port']}",
        f"    public static let relayRtPort: UInt16 = {schema['relay']['rt_port']}",
```

- [ ] **Step 4: Regenerate, verify, commit**

```bash
python3 tools/gen_contract.py
python3 tools/test_gen_contract.py
bash tools/check_contract.sh
tools/test-all.sh
git add contract/dongle-api.json tools/gen_dongle.py tools/test_gen_contract.py \
        firmware/s3/main/dongle_contract.inc app/AJMiddleCar/Generated/DongleAPI.swift
git commit -m "feat(contract): the dongle's vocabulary gains the relay's ports"
```

---

### Task 2: `wifi_state` — the join, as a pure state machine

The spec requires a join that gives up rather than hunting forever: "A `failed` state is reached
and held rather than retried forever; the app decides when to try again by POSTing again. A radio
that hunts for an absent car indefinitely is drawing the phone's battery for nothing, and the
device it is attached to cannot see that happening."

That policy is the testable part of the radio, so it is extracted from the radio. This module
knows nothing of `esp_wifi`; it takes events and answers with a state and one question — should
the caller attempt a connection now.

**Files:**
- Create: `firmware/s3/main/wifi_state.c`, `firmware/s3/main/wifi_state.h`
- Create: `firmware/s3/test/test_wifi_state.c`
- Modify: `firmware/s3/test/Makefile`

**Interfaces:**
- Produces: the types and functions below. Task 3 drives them from `esp_wifi` events.

- [ ] **Step 1: Write the failing tests**

Create `firmware/s3/test/test_wifi_state.c`. Follow `test_net_cfg.c`'s shape — a `main` that runs
named cases and counts failures, no framework:

```c
#include <stdio.h>
#include <string.h>

#include "wifi_state.h"

static int failures;

static void check(int ok, const char *what)
{
    if (!ok) { printf("FAIL: %s\n", what); failures++; }
}

static void test_idle_until_configured(void)
{
    wifi_sm_t sm;
    wifi_state_init(&sm);
    check(sm.state == WIFI_IDLE, "starts idle");
    check(!wifi_state_step(&sm, WIFI_EV_DISCONNECTED), "idle ignores a disconnect");
    check(sm.state == WIFI_IDLE, "idle stays idle without a configuration");
}

static void test_configured_starts_joining(void)
{
    wifi_sm_t sm;
    wifi_state_init(&sm);
    check(wifi_state_step(&sm, WIFI_EV_CONFIGURED), "configuring asks for a connect");
    check(sm.state == WIFI_JOINING, "configuring enters joining");
}

static void test_got_ip_is_connected(void)
{
    wifi_sm_t sm;
    wifi_state_init(&sm);
    wifi_state_step(&sm, WIFI_EV_CONFIGURED);
    check(!wifi_state_step(&sm, WIFI_EV_GOT_IP), "an address does not ask for another connect");
    check(sm.state == WIFI_CONNECTED, "an address means connected");
}

static void test_retries_are_bounded(void)
{
    wifi_sm_t sm;
    wifi_state_init(&sm);
    wifi_state_step(&sm, WIFI_EV_CONFIGURED);
    for (int i = 1; i < WIFI_JOIN_ATTEMPTS; i++) {
        check(wifi_state_step(&sm, WIFI_EV_DISCONNECTED), "a failed attempt retries");
        check(sm.state == WIFI_JOINING, "still joining while the budget lasts");
    }
    check(!wifi_state_step(&sm, WIFI_EV_DISCONNECTED), "the last failure does not retry");
    check(sm.state == WIFI_FAILED, "the budget runs out into failed");
}

static void test_failed_is_held(void)
{
    wifi_sm_t sm;
    wifi_state_init(&sm);
    wifi_state_step(&sm, WIFI_EV_CONFIGURED);
    for (int i = 0; i < WIFI_JOIN_ATTEMPTS; i++) wifi_state_step(&sm, WIFI_EV_DISCONNECTED);
    check(sm.state == WIFI_FAILED, "failed after the budget");
    check(!wifi_state_step(&sm, WIFI_EV_DISCONNECTED), "failed does not retry on its own");
    check(sm.state == WIFI_FAILED, "failed is held, not left");
}

static void test_a_new_configuration_leaves_failed(void)
{
    wifi_sm_t sm;
    wifi_state_init(&sm);
    wifi_state_step(&sm, WIFI_EV_CONFIGURED);
    for (int i = 0; i < WIFI_JOIN_ATTEMPTS; i++) wifi_state_step(&sm, WIFI_EV_DISCONNECTED);
    check(wifi_state_step(&sm, WIFI_EV_CONFIGURED), "a new POST asks for a connect");
    check(sm.state == WIFI_JOINING, "a new POST leaves failed");
}

static void test_a_dropped_link_rejoins_with_a_full_budget(void)
{
    wifi_sm_t sm;
    wifi_state_init(&sm);
    wifi_state_step(&sm, WIFI_EV_CONFIGURED);
    wifi_state_step(&sm, WIFI_EV_GOT_IP);
    check(wifi_state_step(&sm, WIFI_EV_DISCONNECTED), "a dropped link retries");
    check(sm.state == WIFI_JOINING, "a dropped link goes back to joining");
    /* A link that worked once gets the whole budget again, not the remainder of an old one:
       the car powering off and on is the ordinary case, not an escalating failure. */
    for (int i = 1; i < WIFI_JOIN_ATTEMPTS; i++) {
        check(wifi_state_step(&sm, WIFI_EV_DISCONNECTED), "the budget restarted");
    }
    check(sm.state == WIFI_JOINING, "still joining at the end of a full budget");
}

static void test_renewal_does_not_disturb_connected(void)
{
    wifi_sm_t sm;
    wifi_state_init(&sm);
    wifi_state_step(&sm, WIFI_EV_CONFIGURED);
    wifi_state_step(&sm, WIFI_EV_GOT_IP);
    check(!wifi_state_step(&sm, WIFI_EV_GOT_IP), "a DHCP renewal asks for nothing");
    check(sm.state == WIFI_CONNECTED, "a DHCP renewal leaves the state alone");
}

static void test_names_are_the_contract_s(void)
{
    wifi_sm_t sm;
    wifi_state_init(&sm);
    check(strcmp(wifi_state_name(&sm), DONGLE_STATE_IDLE) == 0, "idle spells the contract's word");
    wifi_state_step(&sm, WIFI_EV_CONFIGURED);
    check(strcmp(wifi_state_name(&sm), DONGLE_STATE_JOINING) == 0, "joining spells it");
    wifi_state_step(&sm, WIFI_EV_GOT_IP);
    check(strcmp(wifi_state_name(&sm), DONGLE_STATE_CONNECTED) == 0, "connected spells it");
    wifi_state_init(&sm);
    wifi_state_step(&sm, WIFI_EV_CONFIGURED);
    for (int i = 0; i < WIFI_JOIN_ATTEMPTS; i++) wifi_state_step(&sm, WIFI_EV_DISCONNECTED);
    check(strcmp(wifi_state_name(&sm), DONGLE_STATE_FAILED) == 0, "failed spells it");
}

int main(void)
{
    test_idle_until_configured();
    test_configured_starts_joining();
    test_got_ip_is_connected();
    test_retries_are_bounded();
    test_failed_is_held();
    test_a_new_configuration_leaves_failed();
    test_a_dropped_link_rejoins_with_a_full_budget();
    test_renewal_does_not_disturb_connected();
    test_names_are_the_contract_s();
    if (failures) { printf("%d check(s) failed\n", failures); return 1; }
    printf("wifi_state: all checks passed\n");
    return 0;
}
```

- [ ] **Step 2: Add it to the host build**

In `firmware/s3/test/Makefile`, add a `test_wifi_state` target beside `test_net_cfg`, built the
same way, and add it to `all` and to `run`. Keep the existing flags exactly:
`-I../main -Wall -Wextra -Werror -std=c11`.

- [ ] **Step 3: Run it and watch it fail**

Run: `make -C firmware/s3/test run`
Expected: the compile fails — `wifi_state.h` does not exist.

- [ ] **Step 4: The header**

Create `firmware/s3/main/wifi_state.h`:

```c
#ifndef WIFI_STATE_H
#define WIFI_STATE_H

#include <stdbool.h>
#include <stdint.h>

#include "dongle_contract.inc"

/* The join, as a policy separate from the radio that enacts it.
 *
 * Pure: no ESP-IDF, so the retry budget and the give-up rule are host-tested rather than
 * reasoned about on a bench with a car that may or may not be switched on. wifi_sta.c holds
 * the esp_wifi calls and no policy of its own.
 *
 * The rule this exists to enforce: a join that fails gives up and stays given up. A radio that
 * hunts for an absent car indefinitely drains the phone it is plugged into, and the phone
 * cannot see it happening. The app restarts the attempt by POSTing /net again. */

typedef enum {
    WIFI_IDLE = 0,   /* nothing configured yet — the dongle has never been told a network */
    WIFI_JOINING,
    WIFI_CONNECTED,  /* associated AND addressed: the relays have a gateway to aim at */
    WIFI_FAILED,     /* the budget ran out; held until a new configuration arrives */
} wifi_state_t;

typedef enum {
    WIFI_EV_CONFIGURED,    /* a POST /net arrived with a network to join */
    WIFI_EV_DISCONNECTED,  /* association lost, or an attempt failed */
    WIFI_EV_GOT_IP,        /* DHCP completed — the only event that means "usable" */
} wifi_ev_t;

/* Five attempts is a judgement, not a measurement: enough to ride out a car still booting its
 * softAP, few enough that a car which is simply off costs seconds rather than a battery. */
#define WIFI_JOIN_ATTEMPTS 5

typedef struct {
    wifi_state_t state;
    uint8_t      attempts;  /* consumed attempts in the current budget */
} wifi_sm_t;

void wifi_state_init(wifi_sm_t *sm);

/* Feed one event. Returns true when the caller should attempt a connection now — the module
 * never calls the radio itself, which is what keeps it pure and testable. */
bool wifi_state_step(wifi_sm_t *sm, wifi_ev_t ev);

/* The state as GET /status spells it, straight from the generated contract. */
const char *wifi_state_name(const wifi_sm_t *sm);

#endif /* WIFI_STATE_H */
```

- [ ] **Step 5: The implementation**

Create `firmware/s3/main/wifi_state.c`:

```c
#include "wifi_state.h"

void wifi_state_init(wifi_sm_t *sm)
{
    sm->state = WIFI_IDLE;
    sm->attempts = 0;
}

bool wifi_state_step(wifi_sm_t *sm, wifi_ev_t ev)
{
    switch (ev) {
    case WIFI_EV_CONFIGURED:
        /* From any state, including FAILED: a new configuration is the app saying "try again",
           and it restores the whole budget rather than the remainder of a spent one. */
        sm->state = WIFI_JOINING;
        sm->attempts = 0;
        return true;

    case WIFI_EV_GOT_IP:
        /* An address is the only evidence that matters: association alone leaves the relays
           with no gateway to aim at. Idempotent, so a DHCP renewal changes nothing. */
        sm->state = WIFI_CONNECTED;
        sm->attempts = 0;
        return false;

    case WIFI_EV_DISCONNECTED:
        if (sm->state == WIFI_IDLE || sm->state == WIFI_FAILED) {
            return false;  /* nothing configured, or already given up — do not retry */
        }
        if (sm->state == WIFI_CONNECTED) {
            /* A link that worked once and dropped gets a fresh budget: a car switched off and
               on again is the ordinary case, not an escalating failure. */
            sm->state = WIFI_JOINING;
            sm->attempts = 1;
            return true;
        }
        sm->attempts++;
        if (sm->attempts >= WIFI_JOIN_ATTEMPTS) {
            sm->state = WIFI_FAILED;
            return false;
        }
        return true;
    }
    return false;  /* unreachable for the enum above; keeps -Werror quiet about the return */
}

const char *wifi_state_name(const wifi_sm_t *sm)
{
    switch (sm->state) {
    case WIFI_JOINING:   return DONGLE_STATE_JOINING;
    case WIFI_CONNECTED: return DONGLE_STATE_CONNECTED;
    case WIFI_FAILED:    return DONGLE_STATE_FAILED;
    case WIFI_IDLE:      break;
    }
    return DONGLE_STATE_IDLE;
}
```

- [ ] **Step 6: Run the tests, then the suite, then commit**

```bash
make -C firmware/s3/test run
tools/test-all.sh
git add firmware/s3/main/wifi_state.c firmware/s3/main/wifi_state.h \
        firmware/s3/test/test_wifi_state.c firmware/s3/test/Makefile
git commit -m "feat(s3): the join gives up, and says so — as a testable policy"
```

---

### Task 3: `wifi_sta` — the radio

The glue. It owns `esp_wifi`, feeds the state machine, and publishes three things the rest of the
firmware needs: the state name, the RSSI, and the gateway the relays aim at.

**Files:**
- Create: `firmware/s3/main/wifi_sta.c`, `firmware/s3/main/wifi_sta.h`
- Modify: `firmware/s3/main/status_api.c`, `firmware/s3/main/net_api.c`,
  `firmware/s3/main/main.c`, `firmware/s3/main/CMakeLists.txt`,
  `firmware/s3/sdkconfig.defaults`, `firmware/s3/README.md`

**Interfaces:**
- Consumes: `wifi_state.h` (Task 2); `net_cfg_t` and `net_api_current` (already present).
- Produces:
  - `esp_err_t wifi_sta_start(void);`
  - `void wifi_sta_join(const net_cfg_t *cfg);`
  - `const char *wifi_sta_state_name(void);`
  - `int8_t wifi_sta_rssi(void);`
  - `bool wifi_sta_gateway(uint32_t *out_be);` — the gateway as a network-order IPv4 address;
    false until connected. Tasks 4 and 5 call this to learn where to forward.
  - `void wifi_sta_on_connected(void (*cb)(void));` — invoked once each time the state reaches
    connected, so the relays can (re)aim without polling.

- [ ] **Step 1: Station configuration**

Append to `firmware/s3/sdkconfig.defaults`:

```
# The S3 has its own radio — unlike the P4, which has none and drives a C6 over SDIO. Nothing
# here is a co-processor; esp_wifi talks to silicon on the same die.
#
# Station only. The dongle never becomes an AP: the phone reaches it over USB, and a softAP
# would be a second radio interface for the API guard to worry about and for nobody to use.
CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=8
CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM=16
CONFIG_ESP_WIFI_TX_BUFFER_TYPE=1
CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM=16

# The relays forward between two interfaces, so lwIP must be willing to hold more sockets than
# a single-purpose device needs: the HTTP server, the TCP relay's pool and its upstream halves,
# and one UDP socket per real-time session.
CONFIG_LWIP_MAX_SOCKETS=16
```

- [ ] **Step 2: The header**

Create `firmware/s3/main/wifi_sta.h`:

```c
#ifndef WIFI_STA_H
#define WIFI_STA_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "net_cfg.h"

/* The radio, as a station. Holds esp_wifi and no policy: the retry budget and the give-up rule
 * live in wifi_state.{c,h}, which is pure and host-tested.
 *
 * The dongle learns nothing about the car here. It is handed an opaque SSID and password by
 * net_api and joins whatever answers to them; where to forward traffic afterwards comes from the
 * gateway DHCP gave us, because a softAP's gateway is the softAP. */

/* Bring up the station. Joins immediately if a configuration is already stored. Safe to call
 * once, from app_main, after nvs_flash_init and esp_event_loop_create_default. */
esp_err_t wifi_sta_start(void);

/* Join this network, restarting the attempt budget. net_api calls this when a POST /net changed
 * the stored value — an unchanged POST must not restart a working radio. */
void wifi_sta_join(const net_cfg_t *cfg);

/* GET /status's `net.state`, spelled by the generated contract. */
const char *wifi_sta_state_name(void);

/* GET /status's `net.rssi`. 0 when not connected — the dongle is a station and reads its own
 * receiver, so unlike the car this is a real measurement whenever it is non-zero. */
int8_t wifi_sta_rssi(void);

/* The gateway of the joined network, in network byte order. False until connected. This is the
 * relays' destination, and the only reason the dongle needs no compiled-in car address. */
bool wifi_sta_gateway(uint32_t *out_be);

/* Register a callback invoked each time the station reaches connected. Called from the event
 * task; keep it short. One slot, set before wifi_sta_start. */
void wifi_sta_on_connected(void (*cb)(void));

#endif /* WIFI_STA_H */
```

- [ ] **Step 3: The implementation**

Create `firmware/s3/main/wifi_sta.c`. Requirements, in the order they matter:

1. `wifi_sta_start` does: `esp_netif_create_default_wifi_sta()`, `esp_wifi_init` with
   `WIFI_INIT_CONFIG_DEFAULT()`, register a handler for `WIFI_EVENT` (any id) and one for
   `IP_EVENT_STA_GOT_IP`, `esp_wifi_set_mode(WIFI_MODE_STA)`, `esp_wifi_set_storage(WIFI_STORAGE_RAM)`
   (the configuration's home is our own NVS blob, not esp_wifi's), `esp_wifi_start()`. Then, if
   `net_api_current()` returns a configuration, call `wifi_sta_join` on it.
2. **A mutex guards the state machine and the gateway.** They are written from the event task and
   read from the HTTP task. Create it before `esp_wifi_start`. Follow `car.c`'s idiom in the car's
   firmware: a bounded wait, never an infinite one, so a stuck holder cannot wedge a caller.
3. `WIFI_EVENT_STA_DISCONNECTED` → `wifi_state_step(WIFI_EV_DISCONNECTED)`; if it returns true,
   `esp_wifi_connect()`. Log the transition into `WIFI_FAILED` at `ESP_LOGW` with the reason code
   from the event data — a car that is off and a password that is wrong look identical in
   `/status`, and the console is where they stop looking identical.
4. `WIFI_EVENT_STA_START` → `esp_wifi_connect()` only if the state machine is already in
   `WIFI_JOINING`.
5. `IP_EVENT_STA_GOT_IP` → store `event->ip_info.gw`, `wifi_state_step(WIFI_EV_GOT_IP)`, log the
   address and gateway, then invoke the connected callback **outside** the mutex.
6. `wifi_sta_join` copies the credentials into a `wifi_config_t`, calls `esp_wifi_set_config`,
   `wifi_state_step(WIFI_EV_CONFIGURED)`, then `esp_wifi_disconnect()` followed by
   `esp_wifi_connect()`. The disconnect is what makes a re-join to a *different* network work;
   ignore its error when already disconnected.
7. `wifi_sta_rssi` uses `esp_wifi_sta_get_ap_info`; return 0 unless it returns `ESP_OK`.

The credentials are copied with `strncpy` into `wifi_config_t.sta.ssid`/`.password`, which are
`uint8_t[32]` and `uint8_t[64]`. `net_cfg_t` already bounds them to 32 and 63, so the copy fits —
say so in a comment rather than leaving the reader to check.

- [ ] **Step 4: `/status` stops lying**

In `firmware/s3/main/status_api.c`, replace the hard-coded `DONGLE_STATE_IDLE` and `0`:

```c
                     "\"" DONGLE_KEY_NET_STATE "\":\"%s\","
                     "\"" DONGLE_KEY_NET_RSSI "\":%d}}",
                     app->version, app->idf_ver, s_rollback ? "true" : "false", ssid_esc,
                     wifi_sta_state_name(), (int)wifi_sta_rssi());
```

Delete the comment that explains why `state` is always `idle` — it stops being true here — and
replace it with one sentence saying `rssi` is a real reading from the dongle's own receiver, 0
when not connected.

**Recheck the buffer.** The comment above `char body[320]` itemises a worst case of 235. The two
new fields replace a fixed `"idle"` (4) with up to `"connected"` (9) and a fixed `0` (1) with up
to `-128` (4): +8 bytes, so 243 of 320. Update the arithmetic in the comment; do not leave it
stating a number that is no longer the worst case.

- [ ] **Step 5: A changed `POST /net` starts a join**

In `firmware/s3/main/net_api.c`, where the handler has decided the posted value differs from the
stored one and has written NVS, call `wifi_sta_join(&cfg)`. Where it decided the value is
unchanged, call nothing — the spec is explicit: "A `POST` whose body matches the stored value does
not rewrite flash and does not restart a radio that is already connected — so the app may send it
unconditionally, and does."

- [ ] **Step 6: Start it**

In `firmware/s3/main/main.c`, after `net_api_load()` and before `status_api_start()`:

```c
    ESP_ERROR_CHECK(wifi_sta_start());
```

It must precede the server so `/status` can never be asked before the station exists, and it must
stay above the mark-valid block, which remains the last thing `app_main` does.

- [ ] **Step 7: Build, measure, record**

Add `"wifi_sta.c"` and `"wifi_state.c"` to `SRCS` and `esp_wifi` to `PRIV_REQUIRES` in
`firmware/s3/main/CMakeLists.txt`, then:

```bash
source tools/env-p4.sh && (cd firmware/s3 && idf.py build)
```

The spec asks for this number rather than an assumption: "The app is 395 KB today against a 4 MB
slot; the station and its stack will roughly double it. Comfortable, and worth measuring rather
than assuming, because the earlier 1 MB partition would have been tight." Record the built size
in `firmware/s3/README.md`'s bench table as an observed fact, with the slot size beside it.

- [ ] **Step 8: Suite, then commit**

```bash
tools/test-all.sh
git add firmware/s3/main/wifi_sta.c firmware/s3/main/wifi_sta.h \
        firmware/s3/main/status_api.c firmware/s3/main/net_api.c firmware/s3/main/main.c \
        firmware/s3/main/CMakeLists.txt firmware/s3/sdkconfig.defaults firmware/s3/README.md
git commit -m "feat(s3): the dongle joins the network it was told to join"
```

---

### Task 4: `relay_udp` — the real-time channel

The harder of the two relays, because UDP has no connection to lean on. The dongle must decide
for itself when a phone's session is over, and the car's telemetry must find its way back to a
source port the phone chose.

**The shape.** One socket bound to `192.168.7.1:4210` faces the phone. Each distinct phone
source (address, port) gets its own socket facing the car, bound to an ephemeral port and
connected to `gateway:4210`. Replies therefore arrive on a socket that identifies the session
with no lookup and no ambiguity. A single task selects over the phone-facing socket and every
live car-facing one.

**Files:**
- Create: `firmware/s3/main/udp_sess.c`, `firmware/s3/main/udp_sess.h` (**pure**)
- Create: `firmware/s3/main/relay_udp.c`, `firmware/s3/main/relay_udp.h`
- Create: `firmware/s3/test/test_udp_sess.c`; modify `firmware/s3/test/Makefile`
- Modify: `firmware/s3/main/main.c`, `firmware/s3/main/CMakeLists.txt`

**Interfaces:**
- Consumes: `DONGLE_RELAY_RT_PORT` (Task 1); `wifi_sta_gateway` and `wifi_sta_on_connected` (Task 3).
- Produces: `esp_err_t relay_udp_start(void);`

- [ ] **Step 1: Write the failing tests for the pure half**

Create `firmware/s3/test/test_udp_sess.c`, in `test_net_cfg.c`'s style. Cover: a new peer takes a
free slot; the same peer returns the same slot; a different port is a different session; the
table fills and then evicts the least recently used; expiry frees a slot; expiry leaves a fresh
session alone; a touch moves a session's deadline. Use explicit `now_ms` values — the module takes
time as an argument precisely so the tests need no clock.

- [ ] **Step 2: The pure header**

Create `firmware/s3/main/udp_sess.h`:

```c
#ifndef UDP_SESS_H
#define UDP_SESS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Which phone is on the other end of a relayed real-time session, and when it was last heard
 * from. Pure: no sockets, no ESP-IDF, and time arrives as an argument, so the eviction and
 * expiry rules are host-tested instead of observed on a bench during a drive.
 *
 * The table is small on purpose. One phone drives one car; the extra slots exist so that a
 * phone which reconnects from a new source port does not have to wait out the old session's
 * timeout before it can drive. */
#define UDP_SESS_MAX 4

/* Ten seconds. The app streams commands at 10 Hz and the car answers at 5 Hz, so a live session
 * is never quiet for more than a fraction of a second; anything silent this long has ended. Long
 * enough that a stalled phone does not lose its slot mid-drive, short enough that a table of
 * four cannot be exhausted by abandoned sessions in any realistic session. */
#define UDP_SESS_IDLE_MS 10000u

typedef struct {
    uint32_t addr;     /* the phone's address, network byte order */
    uint16_t port;     /* the phone's source port, host byte order */
    uint32_t last_ms;
    bool     used;
} udp_sess_t;

typedef struct {
    udp_sess_t s[UDP_SESS_MAX];
} udp_sess_table_t;

void udp_sess_init(udp_sess_table_t *t);

/* The index of this peer's session, creating it if new and evicting the least recently used
 * slot when the table is full. Never fails: a relay that refuses a packet because its table is
 * full would drop a drive rather than an abandoned session. */
int udp_sess_touch(udp_sess_table_t *t, uint32_t addr, uint16_t port, uint32_t now_ms);

/* The index of this peer's session, or -1. Does not create and does not update the deadline. */
int udp_sess_find(const udp_sess_table_t *t, uint32_t addr, uint16_t port);

/* Free every slot silent for longer than idle_ms. Returns a bitmask of the freed indices so the
 * caller can close exactly those sockets — the pure module names no socket, and the caller
 * needs to know which of its own to close. */
uint32_t udp_sess_expire(udp_sess_table_t *t, uint32_t now_ms, uint32_t idle_ms);

#endif /* UDP_SESS_H */
```

- [ ] **Step 3: The pure implementation, then green tests**

Write `udp_sess.c` to satisfy the header and the tests. Eviction picks the smallest `last_ms`
among used slots. `udp_sess_expire` returns `1u << i` for each slot it frees.

Add `test_udp_sess` to `firmware/s3/test/Makefile` beside the other two, then
`make -C firmware/s3/test run`.

- [ ] **Step 4: The relay**

Create `firmware/s3/main/relay_udp.{c,h}`. `relay_udp_start()` creates one task. The task:

1. Waits until `wifi_sta_gateway()` succeeds — before that there is nowhere to forward. Re-reads
   it whenever the connected callback fires, and closes every car-facing socket when it changes,
   because a session aimed at a stale gateway is worse than no session.
2. Binds the phone-facing socket to `DONGLE_HOST:DONGLE_RELAY_RT_PORT`. **Bind to the address,
   not `INADDR_ANY`** — that is what keeps the relay off the car's network, and it is why the
   relays need no guard of their own while the HTTP server does.
3. `select()` over the phone-facing socket and every live car-facing socket, with a timeout no
   longer than a second so expiry runs even when nothing arrives.
4. Phone → car: `recvfrom`, `udp_sess_touch`, create the session's car-facing socket if the slot
   was free (`connect()` it to `gateway:DONGLE_RELAY_RT_PORT` so later sends need no address),
   `send`.
5. Car → phone: `recv` on the session's socket, `sendto` the phone recorded in that slot.
6. Each pass: `udp_sess_expire`, closing the sockets whose bits come back set.

A datagram larger than the buffer is truncated by `recvfrom` and must be dropped whole rather
than forwarded short — a half control frame is worse than a missing one. Size the buffer at 1500
and say so.

- [ ] **Step 5: Start it, build, commit**

Add both sources to `SRCS`, start `relay_udp_start()` in `app_main` after `wifi_sta_start()`,
build, run `tools/test-all.sh`, and commit:

```
feat(s3): the real-time channel reaches the car through the dongle
```

---

### Task 5: `relay_tcp` — the car's REST surface

**The shape.** A listener on `192.168.7.1:80`. Each accepted connection takes a slot from a
fixed pool and opens its own connection to `gateway:80`. One task selects over the listener and
both halves of every live slot, pumping bytes in whichever direction has them.

**Pool size is a design number, not a detail** — the spec says so. Four: the app can have a
config POST and a firmware upload in flight at once, and a pool of one would deadlock the second
behind the first. Four leaves room for the browser-style parallelism a REST client may use
without letting a leaked slot starve the pool.

**Files:**
- Create: `firmware/s3/main/relay_tcp.c`, `firmware/s3/main/relay_tcp.h`
- Modify: `firmware/s3/main/main.c`, `firmware/s3/main/CMakeLists.txt`

**Interfaces:**
- Consumes: `DONGLE_RELAY_HTTP_PORT` (Task 1); `wifi_sta_gateway`, `wifi_sta_on_connected` (Task 3).
- Produces: `esp_err_t relay_tcp_start(void);`

- [ ] **Step 1: The header**

Create `firmware/s3/main/relay_tcp.h` declaring `relay_tcp_start(void)`, with a comment carrying
three facts a reader needs: the relay interprets nothing (it is a byte pump, and must never grow
a parser); the destination is the gateway the join produced, never a constant; and the listener
binds the USB address specifically, which is what keeps the car's network from reaching it.

- [ ] **Step 2: The relay**

Create `firmware/s3/main/relay_tcp.c` with a pool of four slots, each holding the two sockets and
a state. The task:

1. Waits for a gateway, as the UDP relay does, and drops every live slot if it changes.
2. Listens on `DONGLE_HOST:DONGLE_RELAY_HTTP_PORT`, `listen(backlog=4)`.
3. `select()` over the listener and every live slot's two sockets.
4. On accept with no free slot: **close the new connection immediately** rather than queueing it.
   A REST client sees a refused connection and retries; a client held open by a relay with no
   capacity sees a hang, which is worse and harder to diagnose. Log it at `ESP_LOGW` — a pool
   that fills is a fact worth seeing on the console.
5. The upstream `connect()` must not block the task: set the socket non-blocking, start the
   connect, and complete it through the same `select()` loop. A car that is powered but slow to
   answer must not stall three other sessions.
6. Either half returning 0 or an error closes both and frees the slot. Do not attempt a
   half-close: the car's REST surface has no use for one, and getting it wrong leaks slots.
7. A 1460-byte buffer per direction, reused per pass — not per slot, since one task pumps one
   direction at a time.

- [ ] **Step 3: Start it, build, commit**

Add the source, start `relay_tcp_start()` in `app_main` after `relay_udp_start()`, build, suite,
commit:

```
feat(s3): the car's REST surface answers through the dongle
```

---

### Task 6: The guard — the dongle's own API answers only on the USB wire

This is the constraint the whole plan has been carrying. `httpd_start` binds `INADDR_ANY` and
`httpd_config_t` in IDF 6.0.2 has no bind-address field, so the moment Task 3 brought up a
station, `POST /net` — which carries a Wi-Fi password — and `POST /ota` — which writes firmware —
became answerable from the car's network. The relays do not have this problem: they bind the USB
address explicitly. The HTTP server cannot, so it gets a guard instead.

`httpd_config_t.open_fn` is the mechanism the spec prefers, "because it refuses the connection
rather than the request".

**Files:**
- Create: `firmware/s3/main/api_guard.c`, `firmware/s3/main/api_guard.h`
- Modify: `firmware/s3/main/status_api.c`, `firmware/s3/main/status_api.h`,
  `firmware/s3/main/ota_api.h`, `firmware/s3/main/CMakeLists.txt`,
  `firmware/s3/verify-on-host.sh`, `firmware/s3/README.md`

**Interfaces:**
- Produces: `esp_err_t api_guard_open(httpd_handle_t hd, int sockfd);` — assigned to
  `httpd_config_t.open_fn`.

- [ ] **Step 1: The guard**

Create `firmware/s3/main/api_guard.{c,h}`. `api_guard_open` calls `getsockname(sockfd, ...)` and
returns `ESP_OK` only when the local address equals `DONGLE_HOST`; otherwise it logs the rejected
peer at `ESP_LOGW` and returns `ESP_FAIL`, which makes `esp_http_server` close the socket before
a single byte of request is parsed.

Use the **local** address, not the peer's. The question is which wire the connection arrived on,
and the local address answers it exactly: a connection through the station lands on the station's
address, whatever the peer claims to be. A peer-address check would be a subnet guess.

- [ ] **Step 2: Install it**

In `status_api_start`, set `cfg.open_fn = api_guard_open;` before `httpd_start`, with a comment
saying what it defends and why the server cannot simply bind one address.

Then rewrite the warnings that this task retires. `status_api.h`'s block and `ota_api.h`'s
paragraph both say a future plan must bring this guard; that plan is this one. Replace the
future tense with what is now true: the server still binds `INADDR_ANY` because IDF offers no
alternative, and `open_fn` is what makes that safe.

- [ ] **Step 3: Prove it on the bench script**

The guard's whole point is a request that must *not* be answered, and the existing script only
tests from the USB side, where everything is allowed. Add to `firmware/s3/verify-on-host.sh`,
inside the `DONGLE ATTACHED` block, a check that the USB side still answers `/status` **and** a
note naming the check that needs the car — a request to the dongle's station address on 8080 must
be refused. Add the matching pending row to `firmware/s3/README.md`'s bench table.

- [ ] **Step 4: Build, suite, commit**

```
feat(s3): the config surface refuses everything that is not the USB wire
```

---

## What this plan does not do

- **The app side.** `CarHost.host` moving to the dongle, the startup sequence, the update gate.
  That is Plan 5, and it needs a simulator rather than a bench.
- **Video.** Out of scope, as the spec says.
- **Any change to the car.** `firmware/p4` is not touched. That is the property the relay exists
  to preserve.

## Bench verification, when hardware is available

Tasks 2 and 4's pure halves are host-tested. Everything else in this plan is unprovable without
a car, and the plan says so rather than implying otherwise. In order, with the car powered:

1. `POST /net` with the car's real SSID and password. `/status` moves `idle` → `joining` →
   `connected` within a few seconds, and `net.rssi` becomes a real negative number.
2. A wrong password: `/status` reaches `failed` and **stays** there. Watch the console for the
   disconnect reason — this is the case where `failed` alone cannot tell you what is wrong.
3. `curl http://192.168.7.1/status` returns the **car's** status document, not the dongle's.
   That is the TCP relay working end to end.
4. Drive from the app pointed at `192.168.7.1`. Commands and telemetry both flow, and telemetry
   keeps flowing for a full minute — the UDP relay's session must not expire under a live stream.
5. From a machine on the car's network, `curl http://<dongle's station address>:8080/status`.
   It must be refused. This is the only test of the guard, and the only one that proves a Wi-Fi
   password and a firmware-write endpoint are not exposed.
6. Record every result in `firmware/s3/README.md`'s table.
