# Mock car

The car's wire, without the car: the real-time UDP channel and the whole REST API, over a
`CarState` that implements the watchdog, the reverse-replay retreat and the actuator
arbiter. It is what makes the app testable without hardware, so it is only worth having
while it behaves like the car — an earlier version of this mock defaulted `/recover` to
off/3000 where the firmware has on/5000, served every client at once and had no watchdog,
which taught every simulator session that a car losing its link simply stops.

## Run

```bash
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
.venv/bin/python mock_car.py
```

It binds `0.0.0.0` and prints the address a phone can reach. Loopback exercises neither
App Transport Security, nor local-network privacy, nor interface pinning, which between
them are most of what breaks on a device — so drive it from a real phone when you can.

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
  whose five config domains are one handler pair registered in a loop over the schema.
- `generated.py` — **generated**. Never hand-edit it; change `contract/car-api.json` and
  run `tools/gen_contract.py`.

`tools/conformance.py http://<host>:<port>` runs the REST request matrix against this mock
or against a real car, and `tools/conformance_rt.py <host>:<port>` does the same for the
UDP channel with real datagrams. `tools/test-all.sh` runs both against a mock it starts
itself (skipped when `.venv` is missing, unless `CONFORMANCE=required`). Since the
2026-08-22 unification (rule 8 of the audit-fix spec) there is one reply shape to assert:
`{"ok":true}` on success, `{"error":"…","field":"…"}` on a rejection, `application/json`
either way — for `/calib*` and `/ota` as well as for the five config domains, so
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
  goodbye included. A *bare* goodbye (`{"seq":n,"bye":1}`, no axes) is a complete
  instruction and is acted on.
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
s.sendto(json.dumps({"proto": 1, "hello": "7f3a91c2"}).encode(), car)
print(s.recvfrom(256)[0])                       # the identity reply
for seq in range(1, 21):                        # 2 s of driving forward
    s.sendto(json.dumps({"seq": seq, "t": 0.5, "y": 0}).encode(), car)
    time.sleep(0.1)
s.sendto(json.dumps({"seq": 21, "t": 0, "y": 0, "bye": 1}).encode(), car)
PY
```

The axes on that goodbye are what the app happens to send; `{"seq": 21, "bye": 1}` is
accepted just the same.

Stop streaming without the `bye` and the mock retreats, exactly as the car does.
