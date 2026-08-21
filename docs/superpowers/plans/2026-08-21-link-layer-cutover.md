# Link Layer — the cutover: UDP everywhere, in one flash

> **For agentic workers:** this is a three-implementation change of one wire. Read `docs/superpowers/specs/2026-08-21-link-layer-rearchitecture.md` first; it is the design. This document is the contract between the three implementations and the division of work.

**Goal:** Replace the WebSocket control channel with UDP on all three sides at once — car, mock, app — and ship it as a single flash plus a single app install. No back-compatibility.

**Why one shot:** the owner asked for it, and the alternative is worse. A staged cutover means a period where the app and the car disagree, and the only way to tell a protocol mismatch from a bug is to have neither. The forced-update gate exists for exactly this.

**Spec:** `docs/superpowers/specs/2026-08-21-link-layer-rearchitecture.md`
**Contract:** `contract/car-api.json` — the port, the deadline, the datagram cap, the protocol version and every frame field name are generated from it into C (`cfg_table.inc`), Swift (`Generated/CarAPI.swift`) and Python (`mock_car/generated.py`). **No implementation may write any of those values as a literal.**

---

## The wire, exactly

UDP, port `RT_PORT` (4210). Every datagram is JSON.

Two size limits, and they answer different questions. The car accepts a datagram of at most
`RT_MAX_COMMAND` (96 bytes) and drops anything larger, malformed, or from a non-owner, without
affecting ownership or the watchdog. Both sides read into a buffer of `RT_MAX_DATAGRAM`
(320 bytes), because a telemetry frame is 119-156 bytes and a receive buffer sized from the
command cap would truncate every one of them.

### App → car

**Session open**, repeated at ~5 Hz until answered:
```json
{"proto":1,"hello":"7f3a91c2"}
```
`hello` is a per-session id, 8 hex characters. The car adopts this sender as its owner and drops
datagrams from every other address until it adopts a different `hello`.

**Command**, 10 Hz:
```json
{"seq":1234,"t":0.50,"y":-0.25}
```
`seq` is a monotonic `uint32`. The car drops any datagram whose `seq` is not newer than the last
accepted one, compared as `(int32_t)(seq - last) > 0` so wraparound is correct. `t` and `y` are
floats in `[-1, 1]`; the car clamps. Formatted with two decimals and a period, never a comma.

**Goodbye**:
```json
{"seq":1235,"t":0,"y":0,"bye":1}
```
Stop, suppress the retreat, drop ownership. Sent when the scene leaves `.active`, when the drive
screen is dismissed, and on teardown. **Ownership is not resumable**: after `bye` the app opens a
new session with a fresh `hello` and a fresh `sid`.

### Session lifecycle — who owns the actuator, and when

This section exists because its absence cost a day. The contract said what `hello` and `bye` mean
for the *link* and said nothing about what they mean for the *actuator*, so three implementations
answered that question three different ways — and two of them then grew tests pinning their answer
as correct.

**Every app→car datagram except `hello` carries `seq`.** A datagram without it is dropped,
including a goodbye. The app has no reason to send one, and accepting it would mean a frame that
bypasses replay protection.

**On adopting a session** (`hello` accepted, from any peer):

1. Release `SAFE` if the car holds it, so the previous session's stop does not outlive it.
2. **Clear the breadcrumb history.** A new session has no path to retrace; retreating along the
   *previous* driver's path is worse than not retreating at all.
3. Reset the sequence gate.
4. Leave the control watchdog **disarmed**. It arms on the first accepted *command*, because that
   is the thing it measures. Arming it at the handshake means a session whose first command is
   still in flight trips it.

A repeat `hello` from the same peer with the same sid is answered but does not re-adopt, so a
retransmitted handshake cannot reset a live session's state.

**On `bye`:**

1. `car_stop(LINK_SRC_SAFE)` — and the result is checked, not discarded.
2. **Release `SAFE` immediately.** Holding it sticky would suppress the retreat, but it also locks
   out OTA, the calibration wizard and the console until an app reconnects — you could background
   the app and then be unable to flash the car over the air.
3. **Clear the breadcrumb history**, which is what actually suppresses the retreat: `any_motion()`
   over an empty history is false, so even if the watchdog later trips, the car stops rather than
   retraces. This is the mechanism, not the sticky grant.
4. Disarm the control watchdog. The silence after a goodbye is not a loss.
5. Drop ownership. The next session needs a fresh `hello`.

The two properties this buys, which no single mechanism gives on its own: not one reverse step
after the driver says stop, and every other actuator source still reachable afterwards.

### Car → app

**Hello reply**, once per adopted session:
```json
{"proto":1,"hello":"7f3a91c2","device":"ajmiddlecar","fw":"v1.0+517"}
```
Identity arrives on the first exchange, over the channel that then carries telemetry. This is what
replaces the app's one-shot `/status` probe as the "is this our car" test.

**Telemetry**, 5 Hz, to the owner's address:
```json
{"seq":88,"rx_fps":10,"rssi":-58,"wdt_trips":0,"uptime_s":812,"heap":200000,
 "calibrated":true,"bus_ok":true,"ctl":"rt"}
```
Field names come from `TELEMETRY_FIELDS` in the schema. `ctl` is the actuator owner:
`rt`/`console`/`calib`/`recover`/`ota`/`safe`/`none`.

### REST, unchanged from B2

`/status` (gains `"proto":1`), `/calib`, `/calib/spin`, `/calib/save`, `/ota`, and the five config
domains. All JSON. Still TCP, still on `esp_http_server`.

---

## Division of work

Three implementations, one wire. They do not reference each other; the schema and this document are
the whole seam.

### C — the mock (`tools/mock_car/`)

- `state.py` — the car's state with no server attached: config from `generated.DOMAINS`, the
  control watchdog, the reverse-replay retreat, `wdt_trips`, `ctl`, `bus_ok`. Host-tested in
  `test_state.py` without aiohttp.
- `mock_car.py` — asyncio UDP endpoint on `RT_PORT` implementing hello/seq/bye and the 5 Hz push,
  plus the existing aiohttp REST server. Binds `0.0.0.0` by default so a real iPhone can drive it
  over real Wi-Fi — loopback exercises neither ATS, local-network privacy nor interface pinning,
  which between them are most of what breaks on a device. Prints its reachable address.
- Impairment flags `--loss-pct`, `--rtt-ms`, `--stall-ms`, deterministic per run.
- Config handling delegates entirely to `generated.validate`. A range literal in `mock_car.py` is
  a bug.
- `tools/conformance.py <base-url>` — one REST request matrix run against the mock or a real car:
  field sets, content types, both ends of every range accepted, one past each end rejected, an enum
  refusing a value outside its set, a missing field rejected rather than partially written. Restores
  what it found. Wired into `tools/test-all.sh` against a mock it starts and stops.

**Why the mock's dishonesty mattered:** its `/recover` default was `off`/3000 where the car's is
`on`/5000, it had no watchdog and no retreat, and it served unlimited clients. Every simulator
session taught that a car losing its link stops. It reverses along its own path for up to five
seconds.

### B3 — the firmware (`firmware/p4/main/`)

- `rt_link.{c,h}` — one task, `recvfrom` with `SO_RCVTIMEO` at 20 ms. The timeout is the tick, so
  three things live in one loop: receive and apply a command, notice silence past `RT_WATCHDOG_MS`,
  and push telemetry every fifth tick. Owner is `(addr, port, sid)` learned from `recvfrom`; a
  datagram from anyone else is dropped until a new `hello` is adopted.
- The watchdog moves here from `watchdog.c`'s FreeRTOS software timer — the task that notices
  silence becomes the task that owns the channel, so no priority-1 timer callback is responsible
  for safety and no client-socket lifetime needs tracking. The pure `watchdog_stale()` survives.
- `car_drive(LINK_SRC_RT, …)` on an accepted command; `watchdog_feed()` on a parsed frame, exactly
  as B2 left it. `bye` → `car_stop(LINK_SRC_SAFE)`, recovery suppressed for that loss, ownership
  dropped.
- **Delete** `ws_control.{c,h}`; set `CONFIG_HTTPD_WS_SUPPORT=n`; drop `/ws` from the handler count.
- `telemetry.c` keeps `telemetry_fields`; the push task moves into `rt_link`.
- `/status` gains `"proto":RT_PROTO`.
- `esp_task_wdt` subscribes `rt_link` and the actuator task.

### D — the app (`app/AJMiddleCar/`)

- `CarTransport` (actor) — the one connection owner. `run()` is `withThrowingTaskGroup { sendLoop;
  receiveLoop }` over an `NWConnection` with `NWParameters.udp` built by `CarNet`; first failure
  `cancelAll()`; reconnect with exponential backoff and jitter. `stop(graceful:)` sends `bye` and
  awaits its send. Absorbs `CarHTTP`'s request path, one in flight per path.
- `CarPath` — two `NWPathMonitor`s; publishes `.wifiUp` / `.noWifi(reason)` / `.localNetworkDenied`.
- `CarLink` (`@MainActor`, `@Observable`) — the one liveness truth. `.live` requires all three: path
  satisfied, session adopted, newest telemetry under a second old. `.wrongCar` comes from the hello
  reply, immediately. Delete `CarConnection.state`, `CarStatus.online`, and the `status.fw != nil`
  latch. `AppFlow` loses `.drive` as a terminal state.
- `CarError` replaces every `nil` return: `.noWiFi(reason) | .denied | .refused | .timeout |
  .http(status, body) | .malformed | .truncated`.
- `ConfigStore` — `.unknown | .loaded | .saving | .failed` per domain, over the generated `Codable`
  structs. **`.unknown` renders as "not read", never as a value.** `CalibClient.spin` returns a
  result and the wizard blocks on failure.
- `ControlIntent` — one command authority; manual pre-empts a trick synchronously, before any
  `await`. `startTrick` performs no I/O.
- `pause()` disappears: leaving `.active` is `stop(graceful: true)`.
- Frames are built from `CarContract`/`TelemetryKey`, never from string literals.

---

## Verification

Every step below must pass before the flash.

| | Check | How |
|---|---|---|
| 1 | Host tests green | `tools/test-all.sh` exits 0 |
| 2 | Firmware builds clean | `idf.py build` exits 0, zero warnings |
| 3 | App builds | `xcodebuild build -scheme AJMiddleCar` succeeds |
| 4 | Conformance | `tools/conformance.py` green against the mock |
| 5 | Mock drives the app | simulator against the mock: hello adopted, telemetry arriving, joystick moves the reported `t`/`y` |
| 6 | Loss tolerance | mock at `--loss-pct 10`: the car keeps driving, because a dropped datagram costs one tick rather than a retransmission timer |
| 7 | Goodbye | backgrounding the app stops the mock's car and leaves `wdt_trips` unchanged |
| 8 | Range loss | stopping the stream without `bye` makes the mock retreat, and `wdt_trips` rises |

Then, on hardware: flash, install the app, drive.

## What this does not do

The launch gate stays as the owner chose — internet before the car, GitHub over whatever path
reaches it. `AccessorySetupKit` joining is **not** in this cutover: it needs the paid account
configured and a device to test against, and it is not on the path between here and driving. It
stays as the next piece of work, with `NEHotspotConfiguration` as the fallback if ASK turns out to
require a Bluetooth rule for discovery.
