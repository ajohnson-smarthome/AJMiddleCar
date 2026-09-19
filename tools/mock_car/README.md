# Mock car

The car's wire, without the car: the real-time UDP channel, the video channel and the REST
API — all of it but `GET /snapshot`, the bench's look at the sensor, which the mock answers
`404` — over a `CarState` that implements the watchdog, the reverse-replay retreat and the
actuator arbiter. It is what makes the app testable without hardware, so it is only worth
having while it behaves like the car — an earlier version of this mock defaulted `/recover`
to off/3000 where the firmware has on/5000, served every client at once and had no watchdog,
which taught every simulator session that a car losing its link simply stops.

## Run

```bash
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
.venv/bin/python mock_car.py
```

It binds `0.0.0.0` and prints the address the LAN can reach. A device build cannot be
pointed at it: on a phone the app addresses the dongle and only the dongle. The mock is
the simulator's car; what only a device can exercise — App Transport Security,
local-network privacy, interface pinning — is exercised against the real dongle and car.

| Flag | |
|---|---|
| `--host` | bind address, default `0.0.0.0`; `127.0.0.1` for a simulator-only session |
| `--port` | REST port, default 8080 |
| `--rt-port` | real-time UDP port; defaults to the contract's, move it only to run a second mock |
| `--device` | the identity to report — change it to exercise the app's wrong-car path |
| `--loss-pct` | drop this percentage of datagrams, each way |
| `--rtt-ms` | add this round-trip latency |
| `--stall-ms` | every 5 s, stop servicing the socket for this long |
| `--seed` | impairment seed; the same seed replays the same *inbound* loss pattern for a client that behaves the same way. Outbound loss depends on how many telemetry pushes preceded the session, so it repeats only for a run driven identically |
| `--rssi` | signal to report, default −58; `0` is the contract's "unavailable", which the app renders differently from a very weak signal |
| `-v` | log every frame instead of one line a second |
| `--video-port` | video UDP port, default from the contract (`VIDEO["port"]`) |
| `--video-sample` | the Annex B clip to loop, default `sample.h264` next to this file |
| `--video-loss-pct` | drop this percentage of outbound video datagrams |
| `--video-reorder-pct` | delay this percentage by one datagram slot |
| `--video-dup-pct` | send this percentage twice |

## Video

The video port is a second UDP endpoint, `video.py`'s `VideoLink`, entirely separate from
the real-time channel — it only *reads* `RTLink.session` to check that a `view` comes from
the live session's owner. A `view` datagram (like the app sends, and `tools/conformance_video.py`
too) starts the stream: `sample.h264` plays in a loop at `VIDEO["fps"]`, one access unit per
datagram burst, chunked exactly as `video_wire.chunks` chunks it on the car. `stream` bumps
once per stream start; `frame` keeps counting across loops, so to the receiver a looped clip
is one long stream. The subscription is soft — no `view` for `VIDEO["subscribe_timeout_ms"]`
and the mock stops sending and resets `car.video_state/fps/kbps` to idle/0/0, same as the
watchdog does for the drive channel.

`view` may carry `key:true` to ask for a keyframe. The clip is only 3 s with one IDR per
loop, so *seeking forward* to the next one could take up to 3 s — past what a real receiver
would tolerate. Instead the mock immediately replays the most recent IDR access unit it has
already sent (a valid IDR with SPS/PPS; the decoder does not care that it repeats) as the very
next frame, without losing its place in the clip.

`--video-loss-pct`, `--video-reorder-pct` and `--video-dup-pct` seed the same kind of
impairment model `rt_link.py`'s `Impairment` uses for the drive channel — seeded from
`--seed`, so a failing run is repeatable. `tools/conformance_video.py` opens a real-time
session, subscribes on the video port, reassembles frames, asks for a keyframe on every
loss, and checks the contract's invariants (every IDR carries SPS/PPS, a requested keyframe
arrives in under 1 s, under 5% of frames lost); `tools/test-all.sh` runs it against a mock
started with 0.3% loss.

`sample.h264` is checked into the repository (≤ 400 KB) so nothing needs `ffmpeg` to run the
mock. Regenerating it does:

```bash
brew install ffmpeg      # only needed to regenerate the clip, not to run the mock
cd tools/mock_car && ffmpeg -y -f lavfi -i "testsrc2=size=1280x960:rate=15" -t 3 \
  -c:v libx264 -profile:v baseline -level 3.1 -pix_fmt yuv420p -bf 0 -b:v 600k \
  -x264-params "keyint=45:min-keyint=45:scenecut=0:bframes=0:repeat-headers=1:annexb=1:nal-hrd=none" \
  -f h264 sample.h264
```

At 600k a P-frame spans 4–6 of `VIDEO["chunk_bytes"]` (1400 B) chunks, and a multi-chunk
frame is lost whenever *any* of its chunks is: per-datagram loss `p` becomes roughly
`1-(1-p)^5` per frame, plus whatever arrives while a requested keyframe is still in flight —
`Receiver` withholds every frame it *does* finish reassembling until that keyframe lands, and
those withheld-but-complete frames count toward neither side of the conformance's loss ratio,
which skews the measured percentage toward the size of whichever frames actually trigger the
few counted drop events (up to the 16-chunk keyframe itself). That makes 2% datagram loss
into 11–28% measured frame loss — the physics of UDP video without FEC, exactly the case the
reassembler and key-on-demand recovery exist for. The bench's own target for the real link is
0.5%, but at that figure the same skew still lands the measured ratio right at the
conformance's 5% edge (confirmed empirically: 6.2%, reproducibly, over a 6 s run) — so
`tools/test-all.sh` runs the mock at 0.3% for a reliable margin (~2.3% measured, consistently)
and treats 0.5%/2% as the figures to reach for by hand with `--video-loss-pct` when exercising
the bench's own numbers directly, not as something this fixed-length, fixed-seed conformance
run can be relied on to clear on every machine.

`tools/conformance_video.py 127.0.0.1 --out /tmp/out.h264` writes the reassembled stream to
a file; `ffplay /tmp/out.h264` opens it.

## What is where

- `state.py` — all the behaviour, with no server attached and no clock of its own: the
  caller passes `now`. Ranges, defaults and deadlines come from `generated.py`, which the
  generator writes from `contract/car-api.json`. A literal in here is a bug.
- `rt_link.py` — the real-time channel: hello / seq / bye, ownership, the 5 Hz push and
  the impairment model. It touches its event loop through `time()` and `call_later()` and
  the network through `transport.sendto()`, so a test supplies all three and drives
  `datagram_received` directly. Stdlib only, like `state.py`, and for the same reason.
- `test_state.py`, `test_rtlink.py` — `python3 test_state.py && python3 test_rtlink.py`.
  Stdlib only: no aiohttp, no sockets, no sleeping.
- `mock_car.py` — plumbing only: it binds the UDP endpoint and the aiohttp REST server,
  whose six config domains are one route, `/config`, that walks the schema.
- `generated.py` — **generated**. Never hand-edit it; change `contract/car-api.json` and
  run `tools/gen_contract.py`.

`tools/conformance.py http://<host>:<port>` runs the REST request matrix against this mock
or against a real car, `tools/conformance_rt.py <host>:<port>` does the same for the
UDP channel with real datagrams, and `tools/conformance_video.py <host>` for the video
channel (above). `tools/test-all.sh` runs all three against a mock it starts itself (skipped
when `.venv` is missing, unless `CONFORMANCE=required`). Since the
2026-08-22 unification (rule 8 of the audit-fix spec) there is one reply shape to assert:
`{"ok":true}` on success, `{"error":"…","field":"…"}` on a rejection, `application/json`
either way — for `/calib*` and `/ota` as well as for the six config domains, so
conformance asserts their bodies and not merely their status codes. The firmware's
plain-text `ok` and `httpd_resp_send_err` bodies are gone; `field` is `""` when the fault
is the body as a whole.

## The two caps

`max_command` is the largest datagram the car will **act on**; anything longer is dropped
by the parser without touching ownership, the sequence window or the watchdog.
`max_datagram` is the receive-buffer size at both ends, and the ceiling on what the car
**sends** — a telemetry frame does not fit inside the command cap, which is the whole
reason there are two. Inbound is capped by the first, outbound by the second. Both
numbers live in `contract/car-api.json` and nowhere else; read them from `generated.RT`.

## Session lifecycle

Who owns the actuator, and when, is the one thing the three implementations answered
three different ways — see "Session lifecycle — who owns the actuator, and when" in
`docs/superpowers/plans/2026-08-21-link-layer-cutover.md`, which is authoritative. In
short, and as `state.py` implements it:

- Every app→car datagram except `hello` carries `seq`. One without it is dropped, a
  goodbye included. A *bare* goodbye (`{"proto":2,"type":"bye","seq":n}`, no axes) is a
  complete instruction and is acted on.
- **Adopting** a session releases SAFE, clears the breadcrumb history, resets the
  sequence gate, and leaves the watchdog **disarmed** — it arms on the first accepted
  command, because that is what it measures. A repeat `hello` from the same peer and sid
  is answered but does not re-adopt.
- **A goodbye** stops through the arbiter (and the result is checked), releases SAFE
  immediately, clears the breadcrumb history, disarms the watchdog and drops ownership.
  The empty history is what suppresses the retreat; a sticky SAFE grant would do it too,
  but it would also lock OTA, the wizard and the console out of the car until an app
  reconnected. A *sticky* holder is the exception (rule 2): while OTA or the calibration
  wizard owns the actuator, the goodbye neither steals nor releases that grant — the
  motors are already stopped, or under the wizard's pulse — and does only its other
  three duties.
- **A watchdog trip clears the arm flag and nothing else — the sequence gate survives**
  (rule 1). Silence proves the stream is dead, but the gate is what stops a
  network-delayed duplicate of a pre-dropout command from being accepted as the resumed
  stream, driving the car at stale stick values and aborting the retreat. A same-session
  stream that resumes carries newer seqs and passes; the gate resets only on adopt and
  on bye.
- **Sessions are mortal** (rule 4). While the watchdog is not armed — after a trip, or
  for a handshake that never commanded — a session more than `session_idle_ms` past its
  last activity ends: ownership cleared, telemetry stopped, sid remembered dead, and the
  breadcrumb path forgotten, which aborts a retreat still in flight. A resuming stream
  says `hello` again.
- **A dead session's sid is remembered** (rule 3), in a ring of four. A hello carrying
  one is answered but not adopted *while a live session exists*, so a network-duplicated
  handshake cannot evict whoever is driving now; with no live session any hello adopts,
  or a client whose session idled out mid-handshake could never get back in.

## Talking to it by hand

```bash
python3 - <<'PY'
import json, socket, time
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.settimeout(1)
car = ("127.0.0.1", 4210)
s.sendto(json.dumps({"proto": 2, "type": "hello", "session": "7f3a91c2"}).encode(), car)
print(s.recvfrom(512)[0])                       # the hello_ack, naming the car
for seq in range(1, 21):                        # 2 s of driving forward
    s.sendto(json.dumps({"proto": 2, "type": "drive", "seq": seq,
                         "throttle": 0.5, "turn": 0}).encode(), car)
    time.sleep(0.1)
s.sendto(json.dumps({"proto": 2, "type": "bye", "seq": 21,
                     "throttle": 0, "turn": 0}).encode(), car)
PY
```

Every datagram names its `type` and carries the contract's `proto` (`2`): a frame without
either, or one still speaking the old `{"hello": …}` / `{"t": …, "y": …}` shape, is dropped
without a reply, exactly as the car drops it. The axes on that goodbye are what the app
happens to send; `{"proto": 2, "type": "bye", "seq": 21}` is accepted just the same.

Stop streaming without the `bye` and the mock retreats, exactly as the car does.
