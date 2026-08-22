# Wire protocol — app ↔ car

The contract between `app/` and `firmware/p4/`. These two never reference each other in code;
this document and `tools/mock_car` are the whole seam. Either side should be reimplementable
from this file alone.

Everything is JSON. The car is a WPA2 softAP; the app talks to the gateway address directly and
never reads the SSID (it has no such entitlement).

| | |
|---|---|
| Network | SSID `AJMiddleCar`, WPA2, password `drive1234` |
| Address | `192.168.4.1` (simulator builds talk to the mock at `127.0.0.1` — same UDP port, REST on `:8080`) |
| Channels | control and telemetry on UDP `4210`; REST on `:80` for configuration and OTA |

The numbers both sides must agree on — the port, the two datagram caps, the rates, the watchdog
deadline, the session-idle limit, the protocol version and every wire key — live in
`contract/car-api.json` and are generated into all four expressions of this contract. No
implementation writes them as literals, and neither does this file except by example.

## The real-time channel — UDP `4210`

Every datagram is a single JSON object. Two size limits answer different questions: the car
accepts an app→car datagram of at most **96 bytes** (`max_command`) and drops anything larger;
both sides read into **320-byte** buffers (`max_datagram`), because a telemetry frame runs
119–156 bytes and a receive buffer sized from the command cap would truncate every one.

Datagrams are parsed strictly, and identically on both implementations: keys are read at the
top level only, numbers follow JSON grammar (no leading `+`, no bare `.` mantissa, no leading
zeros), and a datagram that spells the same top-level key twice is dropped whole. The car scans
flat, the mock runs `json.loads` — anything looser than JSON would drive one and not the other.

### Session open — `hello`, app → car, repeated ~5 Hz until answered

```json
{"proto":1,"hello":"7f3a91c2"}
```

`hello` carries the session id: the app sends 8 hex characters; an acceptor takes 1–15
alphanumerics. `proto` must be an integer: a hello speaking a protocol the car does not is
answered by name (so the mismatch is visible, and the forced-update gate can act on it) but
**not** adopted; a malformed one — `1.5`, a string — is dropped without a reply.

**Every hello is answered**, repeats included — the sender repeats the handshake until it hears
back, so a lost reply must be answerable by the next repeat:

```json
{"proto":1,"hello":"7f3a91c2","device":"ajmiddlecar","fw":"v1.0+517"}
```

Identity arrives on the first exchange, over the channel that then carries telemetry: this
reply, not `/status`, is the app's "is this our car" test. `device` is load-bearing. Both cars
in this family serve this same API at this same address, so a client **must** compare it
against the one car it drives and refuse anything else. Treating a mismatch as "offline" is
wrong: the user has to change networks, not wait.

### Ownership

The car serves one client: the sender of the most recently adopted `hello` owns the session,
and datagrams from every other address are dropped. Last hello wins, from any address — a
second pult that knows the password takes the car silently, and the displaced client is not
notified. That is a recorded deferral, not an oversight (see the cutover plan's post-audit
amendments): the displaced app notices its telemetry going stale within ~3 s and re-hellos, so
two live pults fight in slow motion rather than co-drive.

Three hellos do **not** move ownership: a repeat from the current owner with the same sid
(answered, not re-adopted — a retransmitted handshake must not reset a live session); a hello
whose proto the car does not speak; and — while a session is live — a hello carrying the sid
of a recently ended one, because the car remembers the last few sids that ended by `bye`, by
eviction or by idling out, so a network-delayed duplicate of an old handshake cannot evict a
live driver. When no session is live, any well-formed hello adopts: refusing a dead sid there
would wedge a client whose session idled out mid-handshake, and a stale duplicate displaces
nobody — the phantom session just idles out again.

**On adopting a session** the car releases the previous session's stop-grant, clears the
breadcrumb history (a new session has no path to retrace), resets the sequence gate, and leaves
the control watchdog **disarmed** — it arms on the first accepted command, because that is the
thing it measures.

### Command — app → car, 10 Hz

```json
{"seq":1234,"t":0.50,"y":-0.25}
```

`t` is throttle, `y` is yaw, both floats in `[-1, 1]`, formatted with two decimals and a
period, never a comma; the firmware clamps. `seq` is a monotonic `uint32`; the car drops any
datagram whose `seq` is not newer than the last accepted one, compared as
`(int32_t)(seq - last) > 0` so wraparound is correct. **Every app→car datagram except `hello`
carries `seq`** — one without it is dropped, including a goodbye, because a frame without `seq`
is a frame that bypasses replay protection.

**The client streams the held command continuously at 10 Hz — it does not send events.** Two
reasons, both mandatory:

1. A single frame is a ~40 ms pulse. A motor does not visibly move.
2. The stream *is* the liveness signal. Silence for **300 ms** trips the watchdog.

On the watchdog trip the car does not simply stop: it replays its recent command history in
reverse, negated, to retrace its way back into radio range, aborting the moment a fresh frame
arrives. A client that pauses its stream mid-drive will therefore see the car reverse. Send
`{"seq":…,"t":0,"y":0}` to stop; stop streaming only when disconnecting deliberately.

A trip does **not** clear the sequence gate: a network-delayed duplicate from before the
dropout is still stale and still dropped. A stream that resumes after a dropout resumes with
newer `seq`s and passes the gate. And sessions are mortal: strictly more than **10 s**
(`session_idle_ms`) after its last activity — the last accepted command, or the adoption
itself when none ever followed — a session whose watchdog is not armed is over: ownership
clears, the telemetry push stops, the sid joins the dead-sid list, and resuming takes a fresh
`hello`. Armed silence is the watchdog's world; this clock runs only while the watchdog is
disarmed — after a trip, or after a handshake that never commanded.

### Goodbye — app → car

```json
{"seq":1235,"t":0,"y":0,"bye":1}
```

Stop, suppress the retreat, drop ownership. Sent when the scene leaves `.active`, when the
drive screen is dismissed, and on teardown. On a `bye` the car stops, clears the breadcrumb
history (which is what actually suppresses the retreat — replaying an empty history moves
nothing), disarms the watchdog, and releases its stop-grant immediately, so OTA, the
calibration wizard and the console stay reachable while the app is away. The one exception:
when a flash or a calibration pulse holds the actuator, the goodbye leaves that hold untouched —
a backgrounded app must not hand the motors back mid-flash. **Ownership is not resumable:**
after `bye` the app opens a new session with a fresh `hello` and a fresh sid.

### Telemetry — car → app, 5 Hz

Pushed to the owner's address on the same socket, unsolicited:

```json
{"seq":88,"rx_fps":10,"rssi":-58,"wdt_trips":0,"uptime_s":812,"heap":200000,
 "calibrated":true,"bus_ok":true,"ctl":"rt"}
```

`seq` is the push counter, so a client can drop a reordered datagram. `rx_fps` is control
frames received per second, a direct measure of the uplink. `rssi` is the AP-side signal for
the connected station, `0` when unavailable — clients should fall back to their own latency
measure. `wdt_trips` counts watchdog trips since boot; a rising count means the link is
dropping.

`ctl` names the source that currently owns the actuator — `rt`, `console`, `calib`, `recover`,
`ota`, `safe`, or `none`. It is how a client tells "the car is ignoring me because something
outranks me" from "the car is not hearing me". A car retreating under its own command reports
`recover`, which is the only way to show that honestly.

`bus_ok` is false once a write to the motor driver has failed and has not since succeeded. A
car with `bus_ok: false` is reachable, updatable and undriveable — a state worth distinguishing
from being offline, and the one a car boots into when its I2C bus is unplugged.

A failed push does **not** stop the pushing: a full send buffer is a moment, not a
disconnection. The push stops when the session ends — on `bye`, on eviction, or when the
session idles out.

## `GET /status` — the REST identity line

Still served — for humans, scripts, and the radio report; the app's identity test is the hello
reply, and liveness afterwards comes from telemetry freshness, not from polling this.

```json
{"device":"ajmiddlecar","fw":"v1.0+517","proto":1,
 "seq":88,"rx_fps":10,"rssi":-58,"wdt_trips":0,"uptime_s":812,"heap":200000,
 "calibrated":true,"bus_ok":true,"ctl":"rt",
 "radio":{"fw":"3.0.6","expected":"3.0.6","ok":true}}
```

The identity keys and the telemetry block are spelled from the same schema as the wire's, so a
rename cannot present as a different car. One divergence to know: `/status`'s `rx_fps` is a
per-consumer delta — `0` on the first poll after boot and after a gap of 10 s or more — where
the push's is continuous.

`radio` reports the ESP32-C6 co-processor that provides WiFi. Its image is pinned in `board.h`
and delivered out of band — over SDIO from the host, or over its UART header
(`firmware/c6/README.md`) — never through `/ota`. `ok:false` means the image on the radio is
not the one this firmware expects. Nothing else in the system reports this, so a client should
surface it.

## Configuration — REST

All bodies and responses are JSON. Every value is validated on the car: a malformed body, a
wrong-typed or fractional number, or a value outside its range gets `400` — every domain
rejects, none clamp, and an unrecognised `quad` is refused, not defaulted. Every accepted POST
persists to NVS immediately, and a POST of unchanged values does not rewrite flash.

<!-- generated:endpoints -->
| Endpoint | GET returns | POST body | Ranges |
|---|---|---|---|
| `/ramp` | `{"ramp_ms":…}` | same | `ramp_ms` 0..2000 |
| `/trim` | `{"trim_pct":…}` | same | `trim_pct` -30..30 |
| `/recover` | `{"enabled":…, "window_ms":…}` | same | `enabled` true \| false<br>`window_ms` 1000..10000 |
| `/wheel` | `{"diameter_mm":…, "ppr":…, "gear_x100":…, "quad":…}` | same | `diameter_mm` 20..150<br>`ppr` 1..1000<br>`gear_x100` 100..30000<br>`quad` 1 \| 2 \| 4 |
| `/dims` | `{"track_mm":…, "wheelbase_mm":…}` | same | `track_mm` 60..300<br>`wheelbase_mm` 90..360 |
<!-- /generated:endpoints -->

Calibration is not a config domain and is not generated — each of its endpoints has its
own body shape:

| Endpoint | GET returns | POST body | Range |
|---|---|---|---|
| `/calib` | `{"calibrated":true}` | — | — |
| `/calib/spin` | — | `{"pair":0,"dir":1}` | pair `0..3`, dir `0` reverse / `1` forward; pulses ~0.6 s, and the `200` lands only after the pulse ends — the wizard's "which wheel turned?" must not race a spinning wheel. `409` when a higher-priority source holds the actuator — the wheel did **not** turn, and a client must not advance its wizard |
| `/calib/save` | — | `{"wheels":[{"pair":0,"sign":1},…]}` | exactly 4 entries, order FL, FR, RL, RR; `pair` `0..3` unique, `sign` ±1 |

A successful POST — any POST, `/calib/*` and `/ota` included — answers `{"ok":true}`. A
rejected one answers `4xx` with `{"error":"…","field":"…"}`, where `field` names the offending
key — or is empty when the fault is with the body as a whole. Both carry
`Content-Type: application/json`. The body may arrive in any number of TCP segments; the car
reads until `Content-Length` is satisfied.

`GET /` returns the one-line plain-text identity `<device> <fw>`. There is no web UI.

### What the values mean

- **ramp** — slew-rate limit on acceleration, in ms to full scale. Rise is bounded, fall is
  instant, so stopping is never delayed.
- **trim** — straightness correction. Slows the faster side by this percentage.
- **recover** — the reverse-replay retreat described above; `window_ms` is how far back the
  breadcrumb history reaches.
- **wheel / dims** — geometry, used by the app to draw trajectories and to compute manoeuvres
  such as the donut's diameter. The car stores them; it does not yet compute speed from them.
- **calibration** — which channel pair drives which corner and in which direction. Without it
  the car does not know which wheel is which; `/status`'s `calibrated` is false until saved.

## Firmware update — `POST /ota`

Body is the raw application image (`ajmiddlecar.bin`), sent as a single request. The car stops
the motors, holds the actuator for the whole flash, writes the inactive OTA slot, and reboots
into it; the reply is `{"ok":true}` before the reboot. Images under 4 KB are rejected, and so
are bytes that are not an ESP application image — the magic is checked on the first write, the
whole image at the end. A stalled upload is abandoned after roughly 30 seconds of silence.
While the flash runs the car's REST is effectively down — its one server task is busy
writing — so a client should expect concurrent requests to stall rather than fail fast. On the
next boot the firmware marks the image valid, which cancels the bootloader's rollback — so an
image that cannot boot far enough to do that is rolled back automatically.

The radio co-processor's image is **not** delivered this way — see `/status` above and
`firmware/c6/README.md`.

## Not part of this protocol

The USB console (`mix <t> <y>`) is a local debug REPL on the serial port, not a socket. It is
plain text, and commands sent through it are deliberately exempt from the watchdog so that a
bench session does not stop every 300 ms.
