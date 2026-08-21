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
| `--seed` | impairment seed; the same seed replays the same run |
| `-v` | log every frame instead of one line a second |

## What is where

- `state.py` — all the behaviour, with no server attached and no clock of its own: the
  caller passes `now`. Ranges, defaults and deadlines come from `generated.py`, which the
  generator writes from `contract/car-api.json`. A literal in here is a bug.
- `test_state.py` — `python3 test_state.py`. Stdlib only, no aiohttp, no sockets, no
  sleeping.
- `mock_car.py` — plumbing: the UDP endpoint (hello / seq / bye, 5 Hz telemetry to the
  session's owner) and the aiohttp REST server, whose five config domains are one handler
  pair registered in a loop over the schema.
- `generated.py` — **generated**. Never hand-edit it; change `contract/car-api.json` and
  run `tools/gen_contract.py`.

`tools/conformance.py http://<host>:<port>` runs the REST request matrix against this mock
or against a real car, and `tools/test-all.sh` runs it against a mock it starts itself.

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

Stop streaming without the `bye` and the mock retreats, exactly as the car does.
