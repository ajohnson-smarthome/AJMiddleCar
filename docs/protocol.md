# Wire protocol — app ↔ car

The contract between `app/` and `firmware/car/core/`. These two never reference each other in code;
this document and `tools/mock_car` are the whole seam. Either side should be reimplementable
from this file alone.

Everything is JSON. The car is a WPA2 softAP. The app does not join it: on a device the phone
carries a USB-Ethernet dongle that joins the car's network as a station and relays both
channels below, byte for byte, between the phone and the car — the app addresses the dongle,
and the dongle addresses the car. The dongle never parses a datagram or a request; it moves
them, so everything in this document holds unchanged across it. Its own API, the ports it
listens on and how the app tells it the network's name and password are
`contract/dongle-api.json`'s business, not this file's.

| | |
|---|---|
| Network | SSID `AJMiddleCar`, WPA2, password `drive1234` |
| Address | the car is `192.168.4.1` on its own network; the app reaches it at the dongle's `192.168.7.1`, same ports (simulator builds talk to the mock at `127.0.0.1` — same UDP port, REST on `:8080`) |
| Channels | control and telemetry on UDP `4210`; REST on `:80` for configuration and OTA |

The numbers both sides must agree on — the port, the two datagram caps, the rates, the watchdog
deadline, the session-idle limit, the protocol version and every wire key — live in
`contract/car-api.json` and are generated into all four expressions of this contract. No
implementation writes them as literals, and neither does this file except by example.

## The real-time channel — UDP `4210`

Every datagram is a single JSON object, and every one — either direction — carries two
top-level keys that exist to make the object self-describing: `proto`, an integer, and `type`,
a lowercase word naming the message. A car speaking `proto` 2 drops any datagram whose `proto`
is not 2 — `hello` is the one exception, and only in that it still gets an answer (in the car's
own `proto`, so the mismatch is visible on the wire and the forced-update gate can act on it);
the session is not adopted from it either way. A datagram whose `type` is missing or not one the
car recognises is dropped outright: the type used to be guessed from which keys were present, and
a client that got the guess slightly wrong was dropped with nothing to explain why. HTTP carries
no `type` — the URL already says what the message is — but every JSON body the car or the dongle
returns still carries `proto`, for the same reason a UDP datagram does: a capture, a log, or a
mixed-version bench should never need the source next to it to know which dialect it is reading.

| Datagram | Direction | Fields beyond `proto` and `type` |
|---|---|---|
| `hello` | app → car | `session` (string, 8 hex chars sent, 1–15 alphanumerics accepted) |
| `drive` | app → car | `seq` (monotonic `uint32`), `throttle` (float, `[-1,1]`), `turn` (float, `[-1,1]`) |
| `bye` | app → car | `seq` (monotonic `uint32`) |
| `hello_ack` | car → app | `session` (string, echoed), `device` (group — see below) |
| `telemetry` | car → app | `seq` (`uint32`, push counter), `link`, `motors`, `system`, `video` (groups — see below) |

```jsonc
// app → car
{"proto":2,"type":"hello","session":"7f3a91c2"}
{"proto":2,"type":"drive","seq":1234,"throttle":0.50,"turn":-0.25}
{"proto":2,"type":"bye","seq":1235}
```

Two size limits answer different questions: the car accepts an app→car datagram of at most
**96 bytes** (`max_command`) and drops anything larger; a receiver must be sized for **320
bytes** (`max_datagram`), because a telemetry frame runs up to ~190 bytes and a buffer sized
from the command cap would not fit one.

Datagrams are parsed strictly: keys are read at the top level only, numbers follow JSON
grammar (no leading `+`, no bare `.` mantissa, no leading zeros), and a datagram that spells
the same **recognised** top-level key twice is dropped whole. The two implementations get
there by different routes and are strict to different depths — the car scans flat, so it
detects a repeat only of the keys it reads and never looks at the rest of the object; the
mock runs `json.loads`, so it rejects any duplicate key and anything that is not JSON at
all. Write only well-formed JSON: the mock is the strict side, and a datagram the car
happens to accept beyond it is a divergence, not a licence.

### Session open — `hello`, app → car, repeated ~5 Hz until answered

```json
{"proto":2,"type":"hello","session":"7f3a91c2"}
```

`session` carries the session id: the app sends 8 hex characters; an acceptor takes 1–15
alphanumerics. A `hello` whose `proto` the car does not speak is still answered — the reply
names the car's own `proto`, so the mismatch is visible — but it is **not** adopted; a
malformed one — a non-integer `proto`, a missing `session` — is dropped without a reply, like
any other unparseable datagram.

**Every hello is answered**, repeats included — the sender repeats the handshake until it hears
back, so a lost reply must be answerable by the next repeat:

```json
{"proto":2,"type":"hello_ack","session":"7f3a91c2",
 "device":{"id":"ajmiddlecar","fw":"v1.0+784","build":784,"rolled_back":false}}
```

Identity arrives on the first exchange, over the channel that then carries telemetry: this
reply, not `/status`, is the app's "is this our car" test. `device` is the same object that
`/status` carries — one printer serves both, so a rename cannot present differently on the two
paths — and `device.id` is load-bearing. Both cars in this family serve this same API at this
same address, so a client **must** compare it against the one car it drives and refuse anything
else. Treating a mismatch as "offline" is wrong: the user has to change networks, not wait.
`device.build` (the number after `+` in `fw`, already an integer) and `device.rolled_back` arrive
with the handshake itself, so a client no longer has to visit `/status` to learn whether the
last update survived its first boot.

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

### Command — `drive`, app → car, 10 Hz

```json
{"proto":2,"type":"drive","seq":1234,"throttle":0.50,"turn":-0.25}
```

`throttle` and `turn` are both floats in `[-1, 1]`, formatted with two decimals and a period,
never a comma; the firmware clamps. `seq` is a monotonic `uint32`; the car drops any datagram
whose `seq` is not newer than the last accepted one, compared as `(int32_t)(seq - last) > 0` so
wraparound is correct. **Every app→car datagram except `hello` carries `seq`** — one without it
is dropped, including a goodbye, because a frame without `seq` is a frame that bypasses replay
protection.

**The client streams the held command continuously at 10 Hz — it does not send events.** Two
reasons, both mandatory:

1. A single frame is a ~40 ms pulse. A motor does not visibly move.
2. The stream *is* the liveness signal. Silence for **300 ms** trips the watchdog.

On the watchdog trip the car does not simply stop: it replays its recent command history in
reverse, negated, to retrace its way back into radio range, aborting the moment a fresh frame
arrives. A client that pauses its stream mid-drive will therefore see the car reverse. Send
`{"proto":2,"type":"drive","seq":…,"throttle":0,"turn":0}` to stop; stop streaming only when
disconnecting deliberately.

A trip does **not** clear the sequence gate: a network-delayed duplicate from before the
dropout is still stale and still dropped. A stream that resumes after a dropout resumes with
newer `seq`s and passes the gate. And sessions are mortal: strictly more than **10 s**
(`session_idle_ms`) after its last activity — the last accepted command, or the adoption
itself when none ever followed — a session whose watchdog is not armed is over: ownership
clears, the telemetry push stops, the sid joins the dead-sid list, and resuming takes a fresh
`hello`. Armed silence is the watchdog's world; this clock runs only while the watchdog is
disarmed — after a trip, or after a handshake that never commanded.

### Goodbye — `bye`, app → car

```json
{"proto":2,"type":"bye","seq":1235}
```

`bye` carries only `seq` — no axes. The car already stops as part of ending the session, so a
zeroed `throttle`/`turn` alongside it would say nothing a plain `bye` doesn't. Sent when the
scene leaves `.active` and on teardown, `bye` stops the car, clears the breadcrumb history
(which is what actually suppresses the retreat — replaying an empty history moves nothing),
disarms the watchdog, and releases its stop-grant immediately, so OTA, the calibration wizard
and the console stay reachable while the app is away. The one exception: when a flash or a
calibration pulse holds the actuator, the goodbye leaves that hold untouched — a backgrounded
app must not hand the motors back mid-flash. **Ownership is not resumable:** after `bye` the app
opens a new session with a fresh `hello` and a fresh sid.

### Telemetry — `telemetry`, car → app, 5 Hz

Pushed to the owner's address on the same socket, unsolicited:

```json
{"proto":2,"type":"telemetry","seq":88,
 "link":   {"rx_hz":10,"rssi_dbm":-58,"timeouts":0},
 "motors": {"bus":"ok","calibrated":true,"owner":"remote"},
 "system": {"uptime_s":812,"free_heap":200000},
 "video":  {"state":"idle","fps":0,"kbps":0,"dropped":0}}
```

`seq` is the push counter, so a client can drop a reordered datagram. `link.rx_hz` is `drive`
datagrams received per second, a direct measure of the uplink. `link.rssi_dbm` is the AP-side
signal for the connected station, `null` when it has not been measured — clients should fall
back to their own latency measure. `link.timeouts` counts watchdog trips since boot; a rising
count means the link is dropping.

`motors.owner` names the source that currently owns the actuator — `idle`, `recovering`,
`console`, `remote`, `calibration`, `update`, or `safe_stop`. It is how a client tells "the car
is ignoring me because something outranks me" from "the car is not hearing me". A car retreating
under its own command reports `recovering`, which is the only way to show that honestly.

`motors.bus` is `"down"` once a write to the motor driver has failed and has not since
succeeded, `"ok"` otherwise. A car with `motors.bus: "down"` is reachable, updatable and
undriveable — a state worth distinguishing from being offline, and the one a car boots into
when its I2C bus is unplugged.

A failed push does **not** stop the pushing: a full send buffer is a moment, not a
disconnection. The push stops when the session ends — on `bye`, on eviction, or when the
session idles out.

## The video channel — UDP `4211`

The FPV stream: the car's camera, encoded on-board and cut into UDP chunks the phone
reassembles into H.264 access units. A separate port from the real-time channel, for reasons
that mirror why `4210` exists at all:

- `4210`'s datagram is capped at 96 bytes and parsed by a zero-alloc parser that is the
  firmware's highest-priority job; a video-sized datagram landing there would break both the
  cap and the priority.
- On the dongle, each port runs its own `relay_udp` instance and its own task — video can be
  paced and dropped without touching the code that carries `drive`.
- On the phone it is a different socket and a different module; `RTFrame` and the control path
  it feeds are untouched.

The rule the whole design answers to: **video must never slow the wheel.** `drive` travels
phone → dongle → car and shares no queue with video at all. The reverse direction —
`hello_ack`, telemetry, HTTP replies, and now video — does share real resources: one lwIP
thread and one USB NTB pool on the dongle (both first-in-first-out by the moment each
`sendto` happened), and one 20-slot SDIO queue on the car. On every one of those, video is
what gets rationed: the dongle's video relay is admission-gated toward the phone (below), and
the car's own sender doses its chunks one per millisecond on a timer rather than bursting a
keyframe out in one shot. None of that is a substitute for measuring the actual ceiling on the
bench — see `docs/bringup.md`.

### Wire format — 12-byte header, then the chunk

One compressed frame is cut into chunks of at most `chunk_bytes` (**1400**) bytes, one chunk
per datagram: a 12-byte header, network byte order, followed by the frame's bytes as they come
out of the encoder (Annex B) — the only binary message in this project, and the one place the
v2 rule "`proto` first" is spelled as a single byte rather than a JSON key.

| Offset | Size | Field | Meaning |
|---|---|---|---|
| 0 | u8 | `proto` | this wire format's own version, always **1** (`video.wire_proto`) — a separate number from the JSON envelope's `proto:2` above; anything else and the datagram is dropped |
| 1 | u8 | `flags` | bit 0 set = keyframe (carries SPS/PPS ahead of the frame data); every other bit must be zero |
| 2 | u8 | `stream` | +1 at every stream start; a receiver that sees it change discards whatever it was assembling and waits for a keyframe |
| 3 | u8 | reserved | always `0`; a receiver that sees it set drops the datagram |
| 4 | u16 | `frame` | the encoded-frame counter, from `0` at each stream start, wrapping modulo 2¹⁶ |
| 6 | u8 | `chunk` | 0-based index of this datagram within the frame |
| 7 | u8 | `count` | chunks in the whole frame, `1..255` |
| 8 | u32 | `captured_ms` | the car's own clock at capture — diagnostic and conformance only; the app has no clock shared with the car, so this is not a latency measurement |

**Length invariant:** every chunk but the last is exactly `chunk_bytes` bytes of payload; the
last is `1..chunk_bytes`. A receiver that does not check this assembles a frame of the wrong
length out of a corrupt datagram, so all three implementations enforce it strictly, and the
car obeys it on the way out just as strictly. 1400 was sized like the control datagram's own
constants, from what actually fits: `1400 + 12 header + 8 UDP + 20 IP + 14 Ethernet = 1454`,
under 1500 on every hop this project crosses — Wi-Fi, the P4↔C6 SDIO link (1536), and the
dongle's USB NCM framing. A frame that would need more than 255 chunks (`>357,000` bytes) is
never sent at all — the encoder counts it as an overflow, the same as any other one, and
`video.dropped` rises.

### Receiver rules

Three implementations read this header — `video_wire.h` on the car, `VideoWire.swift` on the
phone, `video_wire.py` in the mock and the conformance tool — and must agree on every one of
the following, not only on the bytes. The vectors in `video.vectors`
(`contract/car-api.json`) pin the header codec across all three; these nine rules, exercised
in `test_video_wire.c` and mirrored in the other two, pin the verdicts the vectors alone
cannot express — ordering, loss, duplicates, and the length invariant.

1. A header that fails to parse (wrong `proto`, a set reserved byte, an unknown flag bit,
   `count == 0`, or `chunk >= count`) is rejected; nothing about the receiver's state changes.
2. Payload length: exactly `chunk_bytes` for every chunk but the last, `1..chunk_bytes` for
   the last. Anything else is rejected.
3. A `stream` the receiver has not seen before — including the very first datagram it ever
   gets — resets it: no frame in progress, no "last" frame remembered, waiting for a keyframe.
4. A frame is already in progress and a different `frame` arrives: older (compared modulo
   2¹⁶, RFC 1982) is ignored — one out-of-order datagram must not cost two frames; newer
   abandons the frame in progress (`dropped` counts up, the receiver goes back to waiting for
   a keyframe) and begins the new one — this call reports a loss, unless the new frame's very
   first chunk also happens to complete it as a keyframe, in which case it reports the frame.
5. No frame in progress, and `frame` is not newer than the last one finished or abandoned:
   ignored.
6. `count` or `flags` disagrees with the frame already in progress: that frame is abandoned,
   reported as a loss.
7. A chunk index already received is a duplicate: ignored, and does not count toward
   completion — this is a bitset, not a counter, so a duplicate cannot fake a finished frame.
8. Every chunk index is in: the frame is assembled. A keyframe clears "waiting for a
   keyframe"; if the receiver is still waiting, the frame is complete but withheld — nothing
   decodable has arrived since the last loss; otherwise it is delivered, `count − 1` full
   chunks plus the last chunk's own length.
9. A frame whose `count × chunk_bytes` would not fit the receiver's buffer is abandoned the
   moment it is recognised, reported as a loss — the one case none of the three
   implementations can wait out.

After any loss, every following frame is withheld, not merely glitched, until the next
keyframe: the phone's decoder has no reference frame to paper over the gap with and would show
corruption rather than say so, so the receiver takes that choice away from it — and a withheld
frame is exactly what asks for a keyframe (below).

### Subscription — `view`

The app subscribes with a datagram of the same shape as `hello`, on the video port:

```json
{"proto":2,"type":"view","session":"7f3a91c2"}
{"proto":2,"type":"view","session":"7f3a91c2","key":true}
```

`view` carries no `seq` — like `hello`, it is a repeat-until-effective message, not a stream
position, and it is parsed by the same `control_proto` (`CT_VIEW`, read from `type` exactly as
`hello`/`drive`/`bye` are). A stray `view` landing on `4210` is harmless: the classifier there
drops anything type-less that is not `hello`.

The car accepts a `view` only when its `session` matches the *real-time* session's current
owner — `rt_link` publishes that sid for the video channel to read. **sid is the only check.**
Through the dongle's relay a `view` arrives from the relay's own address and its video
socket's source port, not from wherever `drive` is coming from, so comparing addresses the way
`drive` effectively can is not available here — recorded as a deliberate choice, not an
oversight: whoever knows the sid gets the stream, no worse than `drive` already is on the same
network and the same password, and a change of address on a live subscription is logged.

The app sends its first `view` immediately after `hello_ack`, again on every reconnect, and
then every `subscribe_ms` (**1000 ms**) for as long as the drive screen is open.
**`subscribe_timeout_ms` (3000 ms)** without one and the stream stops — the encoder closes,
the camera pipeline stops, `video.state` falls back to `idle`. The same happens the instant
the real-time session itself ends (`bye`, eviction, idling out): one clock, two triggers.
Leaving the drive screen is simply not sending `view` any more; the stream dies on its own
within three seconds, which is also why firmware updates do not need to know the video channel
exists at all — the pipeline is stopped by the time one could reach it.

`key:true` on an accepted `view` asks for an out-of-order keyframe (below); the app repeats it
on every following `view` until a keyframe actually arrives, since the ask itself can be lost
just like anything else on this channel. The chunks that follow leave from the exact socket
that accepted the `view` — the car's video socket is bound, not connected, and answers
whoever most recently sent it a valid one; the dongle's own video-relay socket is
`connect()`-ed to `car:4211`, so a chunk arriving there from anywhere else is dropped rather
than forwarded.

### Keyframes

A planned keyframe goes out every `keyframe_s` (**3 s**) — for a viewer subscribing mid-stream,
not for recovery: waiting up to three seconds after an ordinary loss is exactly the freeze
this design exists to avoid. Recovery is on request: a receiver that abandons a frame asks for
one with `key:true`, and the car forces one with `esp_h264_enc_force_idr()`, rate-limited to
at most once every `idr_min_ms` (**250 ms**) so a burst of loss events cannot turn into a burst
of oversized frames. Every keyframe carries its own SPS/PPS, so a receiver starting mid-stream
— or recovering from a loss — never needs an earlier keyframe to make sense of it.

### `GET /snapshot`

A bench route, not part of the app's flow: one JPEG of whatever the camera currently sees,
`image/jpeg`, with no video subscription involved.

- **`streaming`** — `409 busy`, `"stream running"`. On this silicon the hardware JPEG block
  cannot take the stream's YUV420 buffers, and re-encoding a frame through software for a
  debug endpoint is not worth it. A request that loses the race against a stream that is *just
  starting* gets the same `409`, without touching the stream that won.
- **`off`** — `500 internal`, `"camera off"`. No sensor answered at boot; there is no pipeline
  to start.
- **`idle`** — the pipeline starts for this request alone (in UYVY — the JPEG block cannot
  take YUV420 either), frames are discarded for three seconds while AE walks up from its cold
  start (measured: black at 0 s, settled by ~2.7 s), one is encoded, and the pipeline stops
  again — all inside the one request, about 3.5 s in all. Two more
  `500 internal` replies can come out of this same path: `"capture failed"` if a frame does not
  arrive (a `camera_acquire` timeout or another capture error) and `"jpeg failed"` if the
  hardware JPEG block itself rejects the frame.

### Status and telemetry — the `video` group

`video` is the seventh group in `GET /status`, appended after `system`, and the fourth group
telemetry pushes (`link`, `motors`, `system`, `video`) — the same printer serves both, so it
cannot drift between them:

```json
"video": {"state":"idle","fps":0,"kbps":0,"dropped":0}
```

- **`state`** — `off` (no sensor answered at boot; the car drives without one — there is no
  runtime retry, so a physically reconnected camera needs a reboot), `idle` (sensor in
  standby, nobody watching — CSI, ISP and the encoder are all stopped), or `streaming`
  (encoding for the driver).
- **`fps`** — frames encoded in the last second.
- **`kbps`** — kbit sent in the last second.
- **`dropped`** — frames not sent since boot: the sender had not finished the frames before
  it when the next one was due (the ring holds two, and a keyframe leaves at one chunk a
  millisecond), the encoder's output overflowed, or a frame would have needed more than 255
  chunks. Rising while `streaming` means the bitrate or the encoder's QP corridor
  needs to come down, not that anything is broken.

### Configuration — the `video` domain

One field, generated into the domain table below along with the other five — `bitrate_kbps`,
`500..3000`, default `1500`. It is read once, at the next stream start: `video_link` asks for
it when it opens the encoder, not while one is already running, so a change lands on the next
viewer rather than mid-frame. `fps` is not a setting: it is a constant of the contract, because
only a few whole divisors of the sensor's own frame rate make sense, and the firmware is built
against the one it picked (`sensor_fps` 45 ÷ 3 = `fps` 15), not a stored value.

### Through the dongle — `relay.video_*`

On a device, chunks travel through the dongle's own second `relay_udp` instance, not directly:
`contract/dongle-api.json` adds `relay.video_port` (**4211**, forwarded exactly like
`relay.rt_port` — the dongle parses none of it) and `relay.video_max_kbps` (**2500**), the
ceiling its admission gate enforces toward the phone. The gate's window is a fixed
**1000 ms** — tumbling, not sliding: the byte count restarts at each window's start rather
than looking back one second from every datagram. A keyframe (60–100 KB) has to fit inside
one window whole, or the gate would refuse half of every one of them at the top bitrate. The
gate caps the *average*; the burst itself is absorbed by wider buffers on the USB side, not
by the window. A datagram the gate refuses is dropped and counted, never queued.

`GET /status` on the dongle reports three more fields in its `relay` group, all **nullable**
so a dongle running an older build still parses: `video_sessions` (0..4), `video_kbps` (toward
the phone, one decimal), `video_dropped` (chunks not delivered toward the phone since boot —
refused by the gate, or admitted and then refused by the USB side). `null` here means the
adapter predates video, not that nothing is happening.

## `GET /status` — seven groups

Still served — for humans, scripts, and the radio report; the app's identity test is the
`hello_ack` reply, and liveness afterwards comes from telemetry freshness, not from polling
this.

```json
{"proto":2,
 "device": {"id":"ajmiddlecar","fw":"v1.0+784","build":784,"rolled_back":false},
 "link":   {"rx_hz":10,"rssi_dbm":-58,"timeouts":0},
 "motors": {"bus":"ok","calibrated":true,"owner":"remote"},
 "radio":  {"fw":"3.0.6","expected":"3.0.6","state":"ok"},
 "storage":{"reset_at_boot":false},
 "system": {"uptime_s":812,"free_heap":200000},
 "video":  {"state":"idle","fps":0,"kbps":0,"dropped":0}}
```

`device`, `link`, `motors` and `system` are the same groups `hello_ack` and telemetry carry
between them, and `video` is the same group telemetry carries too — one printer per group,
several call sites, so a rename cannot drift between them. The one difference to know: here
`link.rx_hz` is a poll-to-poll window (`0` on the first poll after boot, and after a gap of
10 s or more), where the push's is continuous.

`radio` reports the ESP32-C6 co-processor that provides WiFi. `radio.state` is `ok` when the
C6's firmware equals `radio.expected` (the version this build was made for, derived from the
host's own `esp_hosted` component pin), `mismatch` when it answered with something else, and
`unavailable` when it did not answer at all — in which case `radio.fw` is `null`. That replaces
a bool plus a magic string (`ok:false` with `fw:"unavailable"`) with one word that names all
three cases. The version the radio must run is delivered out of band — over SDIO from the host,
or over its UART header (`firmware/car/modem/README.md`) — never through `/ota`. Nothing else
in the system reports this, so a client should surface it.

`storage.reset_at_boot` is true for the first boot after an NVS format migration erased the
saved config: calibration and every setting are gone, and a client should say so rather than
let the car drive on defaults silently.

## Configuration — REST

All bodies and responses are JSON, and every one carries `proto`. `GET /config` returns every
domain; `POST /config` takes any subset of domains, but each domain present must be complete —
a `wheel` object missing `quadrature` is rejected, not merged field by field with what is
already stored, and a domain the body omits is left untouched. The whole body is validated
before any of it is applied: if one field in one domain is out of range, nothing in the POST
takes effect, not even the domains that were otherwise fine — a malformed body, a wrong-typed or
fractional number, or a value outside its range all get `400`; every domain rejects, none clamp,
and an unrecognised `quadrature` is refused, not defaulted. A successful POST answers with the
full configuration exactly as now held — the same shape `GET /config` returns, not
`{"proto":2,"ok":true}`, because the point of asking is to see what stuck. Every accepted POST
persists to NVS immediately, and a POST of unchanged values does not rewrite flash.

```jsonc
// GET /config → every domain; POST /config ← any subset of domains, each complete
{"proto":2,
 "ramp":     {"rise_ms":300},
 "trim":     {"balance_pct":0},
 "recovery": {"enabled":true,"window_ms":5000},
 "wheel":    {"diameter_mm":65,"encoder_ppr":11,"gear_ratio":9.0,"quadrature":4},
 "chassis":  {"track_mm":130,"wheelbase_mm":210},
 "video":    {"bitrate_kbps":1500}}
```

<!-- generated:endpoints -->
| Domain | Field | Type | Range | Default | Meaning |
|---|---|---|---|---|---|
| `ramp` | `rise_ms` | int | 0..2000 | 300 | time from zero to full scale in ms; 0 disables the ramp |
| `trim` | `balance_pct` | int | -30..30 | 0 | percentage by which the faster side is slowed |
| `recovery` | `enabled` | bool | true \| false | true | retrace on unexpected silence; when false the car stops instead |
| `recovery` | `window_ms` | int | 1000..10000 | 5000 | how far back the breadcrumb history reaches |
| `wheel` | `diameter_mm` | int | 20..150 | 65 | wheel diameter in mm |
| `wheel` | `encoder_ppr` | int | 1..1000 | 11 | encoder pulses per motor-shaft revolution, one channel |
| `wheel` | `gear_ratio` | decimal | 1..300 | 9.0 | gear ratio as a decimal; 1:9 is 9.0 (held as ratio x100 inside) |
| `wheel` | `quadrature` | enum | 1 \| 2 \| 4 | 4 | quadrature edge multiplier |
| `chassis` | `track_mm` | int | 60..300 | 130 | lateral distance between left and right wheel centres |
| `chassis` | `wheelbase_mm` | int | 90..360 | 210 | longitudinal distance between front and rear wheel centres |
| `video` | `bitrate_kbps` | int | 500..3000 | 1500 | target H.264 bitrate in kbit/s; the adapter's USB is the ceiling |
<!-- /generated:endpoints -->

### What the values mean

- **ramp** — slew-rate limit on acceleration, in ms to full scale. Rise is bounded, fall is
  instant, so stopping is never delayed.
- **trim** — straightness correction. Slows the faster side by this percentage.
- **recovery** — the reverse-replay retreat described above; `window_ms` is how far back the
  breadcrumb history reaches.
- **wheel / chassis** — geometry, used by the app to draw trajectories and to compute manoeuvres
  such as the donut's diameter. The car stores them; it does not yet compute speed from them.

## Calibration — `/calibration`

Not a config domain — its three endpoints have their own shapes, and the wheel table speaks in
named corners rather than array position and sign:

```jsonc
// GET /calibration
{"proto":2,"calibrated":true,
 "wheels":[{"corner":"front_left", "pair":0,"inverted":false},
           {"corner":"front_right","pair":1,"inverted":false},
           {"corner":"rear_left",  "pair":2,"inverted":true},
           {"corner":"rear_right", "pair":3,"inverted":false}]}

// POST /calibration ← the same "wheels" array: four corners, each once; pairs 0..3, each once
//                    → the body of GET /calibration, as now stored

// POST /calibration/spin ← {"pair":0,"direction":"forward"}   → {"proto":2,"ok":true} | 409 busy
```

`corner` is one of `front_left`, `front_right`, `rear_left`, `rear_right` — which physical wheel,
named, rather than a position in an array the client has to remember is FL/FR/RL/RR order.
`inverted` replaces a signed `±1`: `true` means the motor pair spins backwards for this corner
and the firmware negates it to compensate. `direction` on `/calibration/spin` is `"forward"` or
`"reverse"`.

`GET /calibration` returns the table the car actually holds, not only whether it has one — the
wizard can show what is stored before overwriting it. `calibrated:false` comes with an empty
`wheels` array.

`POST /calibration` is validated as a whole body, the same way `/config` is: exactly four
entries, each corner named once, each pair `0..3` used once. A repeat of either is `not_allowed`,
not a silent overwrite, and the response is the stored table — not `{"ok":true}` — so the client
sees exactly what took.

`POST /calibration/spin` pulses the named pair for a fixed duration, and the `200` lands only
after the pulse ends — the wizard's "which wheel turned?" must not race a spinning wheel. `409`
means a higher-priority source holds the actuator: the wheel did **not** turn, and a client must
not advance its wizard.

## Errors

Every error reply, on every endpoint, is the same shape, and the HTTP status says how to react:
`400` for a request the car will never accept as it was sent, `409` for one it cannot serve right
now, `500` for one it tried and failed at.

```json
{"proto":2,"error":{"code":"out_of_range","message":"ramp.rise_ms must be 0..2000","field":"ramp.rise_ms"}}
```

`code` is one word from the fixed list below — the one thing a client switches on, and the one
that can be localised; `message` is one English sentence, for a log, not for display; `field` is
a dotted path to the offending key, present whenever a single key is at fault and absent when the
fault is with the body as a whole. Both carry `Content-Type: application/json`, and the body may
arrive in any number of TCP segments — the car reads until `Content-Length` is satisfied.

Car error codes: `bad_json`, `missing_field`, `unknown_field`, `wrong_type`, `out_of_range`,
`not_allowed` (an enum value outside its list; a repeated corner or pair), `busy` (409),
`too_small`, `not_firmware`, `write_failed` (500), `internal` (500).

`GET /` returns the one-line plain-text identity `<device> <fw>`. There is no web UI.

## Firmware update — `POST /ota`

Body is the raw application image (`ajmiddlecar.bin`), sent as a single request. The car stops
the motors, holds the actuator for the whole flash, writes the inactive OTA slot, and reboots
into it; the reply is `{"proto":2,"ok":true}` before the reboot. Images under 4 KB are rejected,
and so are bytes that are not an ESP application image — the magic is checked on the first
write, the whole image at the end. A stalled upload is abandoned after roughly 30 seconds of
silence. While the flash runs the car's REST is effectively down — its one server task is busy
writing — so a client should expect concurrent requests to stall rather than fail fast. On the
next boot the firmware marks the image valid, which cancels the bootloader's rollback — so an
image that cannot boot far enough to do that is rolled back automatically.

The radio co-processor's image is **not** delivered this way — see `/status` above and
`firmware/car/modem/README.md`.

## Not part of this protocol

The USB console (`mix <t> <y>`) is a local debug REPL on the serial port, not a socket. It is
plain text, and commands sent through it are deliberately exempt from the watchdog so that a
bench session does not stop every 300 ms.
