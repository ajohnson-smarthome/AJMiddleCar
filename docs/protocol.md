# Wire protocol — app ↔ car

The contract between `app/` and `firmware/p4/`. These two never reference each other in code;
this document and `tools/mock_car` are the whole seam. Either side should be reimplementable
from this file alone.

Everything is JSON. The car is a WPA2 softAP; the app talks to the gateway address directly and
never reads the SSID (it has no such entitlement).

| | |
|---|---|
| Network | SSID `AJMiddleCar`, WPA2, password `drive1234` |
| Address | `http://192.168.4.1` (simulator builds: `http://127.0.0.1:8080`, the mock) |
| Channels | one WebSocket at `/ws`, plus REST for configuration and OTA |

## Identity handshake

**`GET /status`** is the app's identity probe and its one-shot bootstrap. Liveness afterwards
comes from telemetry freshness, not from polling this.

```json
{"device":"ajmiddlecar","fw":"v1.0+468","rssi":-58,"ws_fps":10,"wdt_trips":0,
 "uptime_s":812,"heap":200000,"calibrated":true,
 "radio":{"fw":"2.11.7","expected":"2.11.7","ok":true}}
```

`device` is load-bearing. Both cars in this family serve this same API at this same address, so
a client **must** compare it against the one car it drives and refuse anything else. Treating a
mismatch as "offline" is wrong: the user has to change networks, not wait.

`radio` reports the ESP32-C6 co-processor that provides WiFi. It is wire-flashed once and its
version is pinned in `board.h`; `ok:false` means the image on the radio is not the one this
firmware expects. Nothing else in the system reports this, so a client should surface it.

## Control — `/ws`, app → car, 10 Hz

```json
{"t":0.50,"y":-0.25}
```

`t` is throttle, `y` is yaw, both floats in `[-1, 1]`; the firmware clamps. Out-of-shape frames,
empty frames and frames over 31 bytes are ignored.

**The client streams the held command continuously at 10 Hz — it does not send events.** Two
reasons, both mandatory:

1. A single frame is a ~40 ms pulse. A motor does not visibly move.
2. The stream *is* the liveness signal. Silence for **300 ms** trips the watchdog.

On the watchdog trip the car does not simply stop: it replays its recent command history in
reverse, negated, to retrace its way back into radio range, aborting the moment a frame arrives.
A client that pauses its stream mid-drive will therefore see the car reverse. Send `{"t":0,"y":0}`
to stop; stop streaming only when disconnecting deliberately.

## Telemetry — `/ws`, car → app, 5 Hz

Pushed on the same socket, unsolicited:

```json
{"rssi":-58,"ws_fps":10,"wdt_trips":0,"uptime_s":812,"heap":200000,"calibrated":true,
 "ctl":"rt","bus_ok":true}
```

`rssi` is the AP-side signal for the connected station, `0` when unavailable — clients should
fall back to their own latency measure. `ws_fps` is control frames received per second, a direct
measure of the uplink. `wdt_trips` counts watchdog trips since boot; a rising count means the
link is dropping.

`ctl` names the source that currently owns the actuator — `rt`, `console`, `calib`, `recover`,
`ota`, `safe`, or `none`. It is how a client tells "the car is ignoring me because something
outranks me" from "the car is not hearing me". A car retreating under its own command reports
`recover`, which is the only way to show that honestly.

`bus_ok` is false once a write to the motor driver has failed and has not since succeeded. A car
with `bus_ok: false` is reachable, updatable and undriveable — a state worth distinguishing from
being offline, and the one a car boots into when its I2C bus is unplugged.

Only one client is served: the socket of the most recent connection wins, and telemetry stops
being pushed to a socket that errors.

## Configuration — REST

All bodies and responses are JSON. Every value is validated on the car. A malformed body, or a
value outside the range of `/ramp`, `/trim`, `/recover` or `/calib/*`, gets `400` with a short
message; `/wheel` and `/dims` instead clamp silently into range, and an unrecognised `quad`
falls back to the default 4. Every accepted POST persists to NVS immediately, and a
POST of unchanged values does not rewrite flash.

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
| `/calib/spin` | — | `{"pair":0,"dir":1}` | pair `0..3`, dir `0` reverse / `1` forward; pulses ~0.6 s. `409` when a higher-priority source holds the actuator — the wheel did **not** turn, and a client must not advance its wizard |
| `/calib/save` | — | `{"wheels":[{"pair":0,"sign":1},…]}` | exactly 4 entries, order FL, FR, RL, RR; `pair` `0..3` unique, `sign` ±1 |

`GET /` returns a one-line plain-text identity. There is no web UI.

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
the motors, writes the inactive OTA slot, and reboots into it; the reply is `ok` before the
reboot. Images under 4 KB are rejected. A stalled upload is abandoned after roughly 30 seconds
of silence. On the next boot the firmware marks the image valid, which cancels the bootloader's
rollback — so an image that cannot boot far enough to do that is rolled back automatically.

The radio co-processor's image is **not** delivered this way. It is flashed by wire; see
`firmware/c6/README.md`.

## Not part of this protocol

The USB console (`mix <t> <y>`) is a local debug REPL on the serial port, not a socket. It is
plain text, and commands sent through it are deliberately exempt from the watchdog so that a
bench session does not stop every 300 ms.
