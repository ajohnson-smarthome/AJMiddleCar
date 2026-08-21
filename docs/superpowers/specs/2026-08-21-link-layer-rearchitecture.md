# Re-architecting the link layer

The layer where `app/AJMiddleCar` and `firmware/p4` meet. This spec replaces the transport, the
authority model on both sides, and the way the contract between them is maintained.

It supersedes `2026-08-21-wifi-pinned-networking.md`, which fixed one real problem — iOS demoting a
no-internet Wi-Fi network out of the general path — and left the rest of the layer standing. That
fix is kept and built on; nothing in it is reverted.

## The problem, as audited

An eleven-reader audit of both sides on 2026-08-21 produced 183 findings, 20 of them critical.
Three were re-verified by hand against the ESP-IDF sources and the working tree, because they change
what the rest of the layer is standing on.

**Telemetry has never worked on the P4.** `ws_control.c:19` captures the client socket in the
`req->method == HTTP_GET` branch, which is the WebSocket handshake. Under ESP-IDF 5.4 — the sibling
car — that branch ran, because the handshake block fell through to `uri->handler(req)`. Under 6.0.2
it does not: `components/esp_http_server/src/httpd_uri.c:362` reads

```c
/* If the request is websocket handshake, then do not call the uri->handler */
return ESP_OK;
```

and on subsequent data frames `httpd_parse.c:698` sets `r->method = 0`, which is not `HTTP_GET`
either. `s_client_fd` is therefore `-1` for the life of the process and `ws_control_send()` returns
on its first line. The app's whole liveness model rests on those frames: `CarStatus.bootstrap()`
latches `online = true` once, the freshness timer clears it a second later, the repeat probe has
already cancelled itself (`CarStatus.swift:44`), and `AJMiddleCarApp.swift:66` drops an opaque
"searching" overlay over `DriveView` that swallows every touch. This, not the network path, is the
better explanation of the symptom the previous spec was written against.

**Ten-hertz control over TCP with a 300 ms deadline does not close.** `sdkconfig` carries
`CONFIG_LWIP_TCP_RTO_TIME=1500`. One lost 802.11 frame head-of-line-blocks the control socket for
1.5 s, the watchdog declares the link lost at 300 ms, and `recovery` starts replaying the path in
reverse. Ordinary packet loss on Wi-Fi presents to the user as the car driving away backwards. No
amount of tuning reconciles a 1.5 s retransmission timer with a 300 ms deadline; the transport is
wrong for the job.

**HEAD does not build.** `CarNet.swift`, `CarHTTP.swift`, `HTTPParse.swift` and `DiagProbe.swift`
are untracked while tracked files call them. XcodeGen globs the directory, so the project file hides
it. Bench diagnostics carrying the literal comment `// ==== TEMPORARY BENCH DIAGNOSTIC — NOT FOR
COMMIT ====` run in the 10 Hz hot path (`CarConnection.swift:127`, `:174`) and at WARN level from a
priority-22 timer callback (`telemetry.c:42`).

The remaining 180 findings are symptoms of eight root causes. Four of them are what this spec is
actually about:

- **Silence is overloaded.** The control channel has one event — a frame arrived — and three
  orthogonal facts are read off it: what the driver wants, whether the driver is still there, and
  whether the car acted. There is no way to say "I am deliberately stopping". Backgrounding the app
  cancels the only transmitter, so 300 ms later the car reverses along its own path with the
  controls off-screen. `/calib/spin`'s `vTaskDelay(600)` (`calib_api.c:55`) blocks the single httpd
  task, so the watchdog trips on every press of Spin. `ws_control.c:53` feeds the watchdog *before*
  `car_drive`, which can return having done nothing (`car.c:48`).
- **Five unarbitrated producers of the actuator target.** `ramp.c`'s `s_target` is written by
  `car_drive`, the console REPL, `car_spin_pair`, OTA's stop and the retreat task.
  `ramp_set_target` is a `memcpy` under a lock: last caller wins, silently, with no record of the
  conflict. "Ramp is the sole PCA9685 writer" is true and well-designed. "Ramp has one commander" is
  false and undesigned.
- **Four independent notions of "connected".** `conn.state` (published, read nowhere),
  `status.online` (telemetry freshness), `status.fw != nil` (a one-shot latch), `flow.phase` — plus
  the firmware's `s_armed`, `s_client_fd` and `ws_fps`. No `NWPathMonitor` gates anything, so Wi-Fi
  off, local-network denied, wrong network and a powered-down car are one indistinguishable radar.
- **The contract exists in four places and is enforced in zero.** Doc, firmware, mock and Swift
  clients, all hand-written, no version field, no conformance test. They already disagree: `/recover`
  defaults are `off/3000` in the mock, `on/5000` in the firmware and `on/3000` in the doc; the doc's
  only concrete radio version is wrong against `board.h:44`; seven POST handlers answer the bare
  string `ok` as `text/html` under a doc that says everything is JSON.

## Decisions taken before designing

These were the owner's calls, and they narrow the design more than anything else here.

| Decision | Consequence |
|---|---|
| Paid Apple Developer Program | `NEHotspotConfiguration` and AccessorySetupKit become available; in-app join replaces the trip to Settings |
| Keep the forced-update gate | Internet stays a precondition, so the app must own *when* it leaves the internet |
| Deployment target iOS 26 | Structured concurrency and the Swift-native Network API replace hand-written glue |
| Retreat only on unexpected silence | The protocol must carry a deliberate goodbye — a wire change |
| Strictly one client, explicit eviction | Ownership is a property of the datagram channel, not a socket table |
| Generated contract | One schema emits the C table, the Swift structs, the mock and the doc |
| Control **and** telemetry on UDP | The WebSocket disappears; `ws_control.c` is deleted, not repaired |

The gate and the join compose better than either does alone. The app runs its gate on whatever
network it launched on, caches the image, and only then joins the car's AP under its own control.
It leaves the internet after the gate has passed, never before, so the gate is always passable and
the user never visits Settings. GitHub traffic is additionally pinned away from Wi-Fi, so an update
check from inside the car's network still reaches LTE when one is available.

## The wire

### Real-time channel — UDP 4210

One datagram socket. Both directions carry state with last-wins semantics, which is what both
directions actually are. JSON stays: `control_proto.c` is already pure, zero-alloc and host-tested,
and only needs new keys.

**Session open.** The app sends `hello` at ~5 Hz until a reply arrives:

```jsonc
→ {"proto":1,"hello":"7f3a91c2"}
← {"proto":1,"hello":"7f3a91c2","device":"ajmiddlecar","fw":"v1.0+468"}
```

`hello` is a random per-session id. The car adopts the sender as owner and drops datagrams from every
other address until it adopts a new `hello`. That is "strictly one client, last connect wins" with
explicit eviction, obtained from `recvfrom` rather than from a socket table.

The reply carries `device` and `fw`. Identity therefore arrives on the first exchange, over the same
channel that later carries telemetry. Today identity comes only from a one-shot `/status` probe that
cancels itself, which is why `status.fw != nil` became a third independent notion of liveness and
why the wrong-car screen dead-ends.

**Command**, app → car, 10 Hz:

```jsonc
{"seq":1234,"t":0.50,"y":-0.25}
```

`seq` is a monotonic `uint32`. The car drops any datagram not newer than the last accepted one,
compared as `(int32_t)(seq - last_seq) > 0` so wraparound is correct. This is the point of leaving
TCP: a reordered or stale command is discarded in one tick instead of blocking the head of the queue
for 1.5 s. `t` and `y` stay floats in `[-1, 1]`; the car clamps.

**Goodbye**, app → car:

```jsonc
{"seq":1235,"t":0,"y":0,"bye":1}
```

Stop the car, suppress the retreat, drop ownership. Sent when the scene leaves `.active`, when the
drive screen is dismissed, and on teardown. This is what un-overloads silence: a deliberate stop is
said in words, and the retreat arms only on *unexpected* silence.

Ownership is not resumable. After `bye` the app opens a new session with a fresh `hello` and a fresh
`sid`; returning to the foreground is a new session, not a continuation of the old one.

**Telemetry**, car → app, 5 Hz, to the owner's address:

```jsonc
{"seq":88,"rx_fps":10,"rssi":-58,"wdt_trips":0,"uptime_s":812,"heap":200000,
 "calibrated":true,"bus_ok":true,"ctl":"rt"}
```

`ws_fps` becomes `rx_fps` — there is no WebSocket left to name. `ctl` is the current actuator owner
(`rt` / `console` / `calib` / `recover` / `ota` / `safe` / `none`), which is the arbiter's state
surfaced. Today, when the car is retreating under its own command, the app has no way to know or
say so. `bus_ok` reports the motor I2C bus, which the car can now survive losing.

Datagrams over 96 bytes, malformed datagrams, and datagrams from a non-owner are dropped without
affecting ownership or the watchdog.

### REST stays on httpd

Same endpoints. Three changes: every response is `application/json` (seven handlers currently answer
the bare string `ok` as `text/html`), request bodies are read in a **loop** over `content_len`
instead of one `httpd_req_recv`, and `/status` carries `"proto":1`.

`/wheel` and `/dims` **reject** out-of-range values with 400 rather than clamping silently. The
firmware and the mock already do this; only the doc claims otherwise. Silent clamping is
incompatible with a client that needs to know whether its value was taken.

The app refuses an unknown `proto` by name rather than mis-parsing it silently.

### The contract is generated

`contract/car-api.json` becomes the source of truth. A generator emits:

| Artefact | Contents |
|---|---|
| `firmware/p4/main/cfg_table.inc` | C descriptor table for the generic config handler |
| `app/AJMiddleCar/Generated/CarAPI.swift` | `Codable` structs and range constants |
| `tools/mock_car/generated.py` | mock handlers and input assertions |
| `docs/protocol.md` | the endpoint table |

CI fails when the generated output differs from what is committed. The four disagreements the audit
found become impossible by construction rather than by convention.

## Firmware

### Task structure

| | Now | After |
|---|---|---|
| Control | inside the `httpd` task, prio 5, shared with OTA and calibration | **`rt_link`**, prio 6, its own UDP socket |
| Telemetry | `esp_timer` callback, prio 22, containing a 5 s SDIO RPC and an NVS+cJSON read | the same `rt_link` loop, on a counter |
| Watchdog | FreeRTOS software timer on `Tmr Svc`, **prio 1** | one line in the `rt_link` loop |
| Actuator | `ramp`, prio 5, five unarbitrated writers | `ramp` private behind `link.c`, no external writers |
| REST + OTA | the same `httpd` task | `httpd` only, with a raised stack |

`rt_link` is `recvfrom` with `SO_RCVTIMEO = 20 ms` in a loop. The timeout is the tick: the task wakes
every 20 ms whether or not a datagram arrived, so "silent for longer than the deadline" is a line in
the same loop, and telemetry is another line on a counter.

That removes three critical findings at once. The task that *notices* silence is the task that
*owns* the channel: no priority-1 timer service responsible for safety, no blocking SDIO RPC from a
priority-22 callback, and no client-socket lifetime to track.

`esp_task_wdt` then subscribes `rt_link` and `ramp`, so a hung task reboots the board instead of
silently ceasing to exist. That is only safe together with the boot-zeroing fix below.

### `link.c` — the actuator arbiter

```c
typedef enum {                    /* numeric order is priority, ascending */
    SRC_CONSOLE = 0, SRC_RT, SRC_CALIB, SRC_RECOVER, SRC_OTA, SRC_SAFE
} link_src_t;

bool       link_drive(link_src_t src, float t, float y);
bool       link_raw  (link_src_t src, const uint16_t duty[8], uint32_t hold_ms);
void       link_release(link_src_t src);
link_src_t link_owner(void);
```

`ramp_set_target` becomes `static` inside `link.c`; no external writer remains. A request from a
source below the current owner is **rejected and logged** (rate-limited), and the owner is published
as telemetry's `ctl`.

Ownership expires. `SRC_RT` holds until the watchdog deadline, `SRC_CALIB` for its `hold_ms`,
`SRC_RECOVER` until released. On expiry the owner drops to none and the target goes to zero.
`/calib/spin` therefore stops being `vTaskDelay(600)` in a handler: it takes ownership for 600 ms,
answers immediately, and releases itself.

`SRC_SAFE` is used in three places — boot, entering OTA, and `bye`. It holds zero until explicitly
released.

The return value is load-bearing on the control path: `rt_link` feeds the watchdog **only when
`link_drive` returned true**. Today `ws_control.c:53` feeds it before `car_drive`, which can return
having done nothing, so the one mechanism that could notice the actuator had stopped responding is
fed by the frames that failed to reach it.

### Four actuator-safety fixes

**The boot stop reaches the chip.** `ramp_task` writes only when `dirty`, and at boot
`s_current == s_target == 0`, so `dirty` is false and **nothing is sent to the PCA9685**. The chip's
registers survive a P4 reset — a panic reboot, `esp_restart()` after OTA, an `esp_hosted` transport
restart — so a car resetting at speed keeps driving while the firmware believes it is stopped. Fix:
write zero to all sixteen channels unconditionally right after `pca9685_init`.

**A failed I2C write no longer diverges permanently.** `s_current[ch]` is updated *before* the write
succeeds, so one NACK leaves the shadow reading zero, `dirty` never rises again, and that motor spins
at its previous duty until power is removed. Fix: update the shadow only after success; on failure
keep `dirty` and retry.

**The I2C bus gets a timeout.** `pca9685.c:107` waits `-1`, forever: a slave holding SDA low freezes
the sole actuator writer permanently. Fix: a bounded timeout with one retry; a persistent failure
raises `bus_ok: false` in telemetry.

**An emergency stop cannot be silently dropped.** `car_drive` currently gives up after 200 ms on a
busy mutex and returns having commanded nothing, so the watchdog's stop can vanish. Fix: the
calibration table stops being read under a mutex. It is immutable; a write publishes a new copy by
pointer swap and readers never block. Both the stall and the failure mode leave the hot path.

### Boot order

```
motor bus (I2C, PCA9685, explicit zeroing)   <- failure is soft, flagged in telemetry
NVS
link_init            (starts ramp; target at zero)
car_init / wheel_init / dims_init
recovery_init
wifi_ap_start
rt_link_start        <- UDP, watchdog, telemetry
http_server_start    <- REST + OTA
console
```

The `ESP_ERROR_CHECK` on the motor bus goes. Today a dead I2C bus means an endless reboot with no
Wi-Fi, no API and no OTA — recoverable only over USB, which is exactly what the bench hit while the
PCA9685 boards were unwired. The car must come up on the network and say `bus_ok: false`.

### Deletions

- `ws_control.{c,h}` entirely — with the fd-capture bug, the undrained oversized frames, and the
  permanent channel loss from a single retryable `EAGAIN` (`ws_control.c:92`).
- `CONFIG_HTTPD_WS_SUPPORT=n`.
- The software timer in `watchdog.c`. The pure `watchdog_stale()` survives and moves into `rt_link`;
  it is already host-tested, including wraparound.
- The bodies of five of the seven `*_api.c` files, replaced by the generic handler over the generated
  table. Registered URI handlers fall from 17 to about 9, and `max_uri_handlers` stops being a mine.
- The `ESP_LOGW("RATE", …)` diagnostic in `telemetry.c`.

### Telemetry gets cheap

`calibrated` is cached in RAM (set at `car_init` and at `/calib/save`) instead of opening NVS and
running `cJSON_Parse` five times a second. RSSI refreshes once a second in the `rt_link` loop rather
than as a 5 s-timeout SDIO RPC from a priority-22 callback. `rx_fps` gets separate accumulators for
the push and for `/status`, so polling `/status` no longer consumes the push's measurement interval.

### Access point

`max_connection = 1` — there is one driver. `WIFI_EVENT_AP_STADISCONNECTED` stops being only a log:
the owner's station leaving is `link_drive(SRC_SAFE, 0, 0)`. Add `esp_wifi_set_ps(WIFI_PS_NONE)` and
a configurable channel.

Separately and **last**: an experiment with a DHCP lease that advertises no default route
(`ESP_NETIF_ROUTER_SOLICITATION_ADDRESS`, which exists and is handled at
`esp_netif_lwip.c:2561`). The address stays reachable on-link, and iOS has no default-route candidate
to demote. This is a hypothesis with a good pedigree, not a plan — the previous firmware-side attempt
at this problem, answering Apple's captive probe, was implemented, flashed and failed. It runs after
everything else works, or its effect cannot be attributed.

## iOS

### Modules and ownership

| Module | Owns | Single responsibility |
|---|---|---|
| `CarNet` | nothing | How a socket is opened. Gains `internetParams()` with `prohibitedInterfaceTypes = [.wifi]` for GitHub |
| `CarPath` | two `NWPathMonitor`s | Interface and permission truth: `unsatisfiedReason`, `.localNetworkDenied` |
| `CarTransport` *(actor)* | the UDP socket, HTTP requests, the reconnect task | **The one connection owner** |
| `CarLink` *(`@Observable`)* | the published `Link` | **The one liveness truth**, composed |
| `ControlIntent` | the command stream | **The one command authority** |
| `ConfigStore` | per-domain state | Configuration as an explicit state machine |
| `UpdateService` | the image cache | Release metadata, cache, OTA progress. One instance |
| `CarJoin` | the AccessorySetupKit session | Joining the car's network and returning from it |

### `CarTransport`

```swift
actor CarTransport {
    func run() async {                            // lives until cancelled
        while !Task.isCancelled {
            do { try await session() } catch { await backoff() }   // exponential + jitter, cap 5 s
        }
    }
    private func session() async throws {
        let sid = SessionID.random()
        try await handshake(sid)                  // hello until the first reply
        try await withThrowingTaskGroup { g in
            g.addTask { try await self.sendLoop() }       // 10 Hz, deadline-based, no drift
            g.addTask { try await self.receiveLoop() }    // telemetry -> AsyncStream
            try await g.next()                           // first failure
            g.cancelAll()
        }
    }
    func stop(graceful: Bool) async { if graceful { await sendBye() } }
}
```

The four independent variables that currently carry the lifecycle — `started`, `timer`, `conn`,
`outbox.socket`, mutated from six call sites — collapse into one task. Two concurrent reconnect loops
cannot exist because `run()` starts them and there is one `run()`. The old socket is provably dead:
`cancelAll()` takes both sides down, and the next `session()` opens with a fresh `sid`.

Two consequences worth recording. The comment at `CarConnection.swift:47` — that
`Timer.scheduledTimer` stops firing while a finger drags the joystick, because the run loop is in
tracking mode — stops applying: under structured concurrency nothing is on the main run loop, so the
failure has no mechanism. And `pause()` disappears as a concept: leaving `.active` is
`stop(graceful: true)`, a goodbye said in words rather than a cancelled transmitter. The car stops in
under 150 ms instead of retreating.

The exact spelling of the iOS 26 Network API (the Swift-native connection type and its message
sequence) is pinned during implementation; the shape above is the contract, and it holds whether the
loops are built on the new API or on `NWConnection` directly.

### `CarLink`

```swift
enum Link {
    case noWiFi(NWPath.UnsatisfiedReason)
    case localNetworkDenied
    case searching
    case wrongCar(device: String)
    case live(Telemetry)
}
```

`.live` requires all three: the path is satisfied, we own the channel, and the newest telemetry frame
is under a second old. The status pill, the signal bars, the searching overlay and `AppFlow` read
only this. `conn.state`, `status.online` and the `status.fw != nil` latch are deleted.

`wrongCar` now comes from the `hello` reply — immediately, over the same channel. The race where
`onChange` misses the initial value and the self-cancelling `/status` probe that made the wrong-car
screen a dead end both disappear with the mechanism.

`CarPath` produces four distinct screens where there is one endless radar today: Wi-Fi off, local
network denied, wrong network, car not answering. Local-network denial is currently undetectable —
its only signal is `unsatisfiedReason == .localNetworkDenied`, which nothing reads.

`AppFlow` loses `.drive` as a terminal state; the drive screen is entered and left according to
`CarLink`, so a car that goes away mid-session is handled rather than latched.

### Joining the car's network

```swift
let descriptor = ASDiscoveryDescriptor()
descriptor.ssid = "AJMiddleCar"
```

The system picker joins the Wi-Fi and grants local-network consent in **one dialog**, which closes
both the "Open Settings" complaint and the silent local-network denial.
`UIApplication.openSettingsURLString` at `ConnectView.swift:23` is deleted — that constant opens the
app's own settings pane by definition and can never do anything else. Leaving the drive session
returns the phone to its previous network.

Order: the gate runs on the launch network, then `CarJoin` moves the phone, then driving. The app
leaves the internet only after the gate has passed.

**Risk to retire first.** Whether AccessorySetupKit covers an accessory with no Bluetooth at all is
not settled here; discovery may require a Bluetooth rule. It is an hour's check on a device, and the
fallback is ready: `NEHotspotConfiguration` does exactly what is needed. Both require the paid
account. That check is the first implementation step, so no screen is built around an API that may
not fit.

### Error model

```swift
enum CarError: Error {
    case noWiFi(NWPath.UnsatisfiedReason), denied, refused
    case timeout(TimeInterval), http(status: Int, body: Data)
    case malformed(String), truncated(got: Int, want: Int)
}
```

This replaces `nil` everywhere. `CarHTTP` currently returns the same `nil` for connect-refused,
unsatisfied path, send error, deadline, truncated stream and unparseable head, and the six domain
clients flatten that to `nil`/`false` at call sites written as `if let v = await X().get() { … }` with
no `else`. Those clients collapse into two generics over the generated structs.

`ConfigStore` holds `.unknown | .loaded | .saving | .failed`. **`.unknown` renders as "not read",
never as a value.** Today a failed GET is indistinguishable from real data: `WheelParamsView` draws
its hardcoded 65/11/2100/4 as the car's configuration, and one stepper tap POSTs the whole record,
overwriting the car's real gear ratio with the app's fallback.

Separately and most consequential: `CalibClient.spin` returns a result and the calibration wizard
**blocks on failure**. It returns `Void` today, so a failed POST looks exactly like a wheel that
turned; four arbitrary taps produce a table `calibration_valid` cannot reject, and the car drives
with swapped wheels while reporting `calibrated: true`.

## Testing

Pure modules are host-tested with `swiftc` and plain `cc`, as the project already does.

- **Swift, host:** frame encoding including `seq` wraparound, the `CarLink` composition truth table,
  `ControlIntent` priority, `ConfigStore` transitions.
- **C, host:** a harness with a fake `pca9685_set_pwm` asserting that a retreat cannot override a
  console `mix`, that a calibration spin is refused while `SRC_RT` holds, that OTA forces `SRC_SAFE`,
  and that boot emits an actual zeroing write.
- **Conformance:** one request matrix run against firmware-on-hardware and against the mock,
  asserting identical status codes, content types, defaults and range rejections.
- **The mock becomes honest:** bound to the Mac's LAN address rather than loopback, speaking UDP,
  serving one client, implementing the watchdog and the retreat, blocking during OTA, and accepting
  `--rtt-ms / --loss-pct / --stall-ms`. Development currently runs against a mock that bypasses ATS,
  local-network privacy and interface pinning and has zero RTT with no loss — it exercises precisely
  what does not break.

## What breaks on the wire

The real-time channel entirely (WebSocket to UDP), REST response content types, `ws_fps` to
`rx_fps`, `"proto":1`, and the semantics of `bye`. It ships as one firmware-and-app pair, as the JSON
cutover did. The forced-update gate the owner chose to keep exists for exactly this.

## Delivery

The design is one layer and only makes sense whole, but it is too large for one implementation plan.
It decomposes into four, plus two small standalone steps:

| | Work | Wire |
|---|---|---|
| 0 | Commit the transport files; delete `DiagProbe`, `countTick()`, the `DIAG` logs and `telemetry.c:42`; raise the deployment target to iOS 26 | — |
| 1 | Verify AccessorySetupKit on hardware; fall back to `NEHotspotConfiguration` if it does not fit | — |
| **A** | `contract/` schema and generator — **first**, or the protocol gets written by hand four times | — |
| **B** | Firmware: `link.c`, the four actuator-safety fixes, soft boot, `rt_link` on UDP, `ws_control` deleted, generic `cfg_api` | **yes** |
| **C** | Mock over UDP on the LAN, watchdog, retreat, impairment flags, conformance suite | — |
| **D** | iOS: `CarTransport` / `CarPath` / `CarLink`, `CarError`, `ConfigStore`, `ControlIntent`, `CarJoin`, one `UpdateService` | **yes** |
| 11 | Experiment: DHCP lease with no default route | — |

B and D ship together with one flash. A precedes both. C can be developed in parallel with B and is
what makes D testable without hardware.

## How we will know it worked

- Ten presses of Spin during a live 10 Hz stream leave `wdt_trips` unchanged.
- Backgrounding mid-drive stops the car in under 150 ms and does not increment `wdt_trips`. Walking
  out of range still produces the retreat.
- Hammering `/status` at 5 Hz alongside the control stream keeps `rx_fps` at 10 ± 1.
- Airplane mode with a cached image: the app joins the car, drives, and the forced update completes
  from cache.
- Denying local network access produces a screen that says so, not a radar.
- A range change in `contract/car-api.json` changes the firmware, the app, the mock and the doc in
  one commit; CI is red if any of them is hand-edited.
- The mock at 10 % packet loss: the car keeps driving, because a dropped datagram costs one tick
  rather than 1.5 s.
