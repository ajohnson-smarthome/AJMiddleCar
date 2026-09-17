# AJMiddleCar

4-wheel RC car on an **ESP32-P4**, driven from a native iOS pult over WiFi.
Sibling project: **AJPicoCar** (`~/VSCode/esp32-c6-car`) — the smaller car on a XIAO ESP32-C6.
Both are alive; they share a protocol and a design language, and their code diverges from
`docs/superpowers/specs/2026-08-19-p4-migration-design.md` onwards.

## Hardware

| Component | Details |
|---|---|
| Board | Waveshare **ESP32-P4-Module-DEV-KIT** — ESP32-P4NRW32, 32 MB PSRAM, 16 MB flash |
| Silicon | ESP32-P4 **revision v1.3** — early, and IDF 6.0 rejects it unless the build selects the `<3.0` family (see `sdkconfig.defaults`) |
| Radio | **ESP32-C6 on the same board**, over SDIO. The P4 has no radio of its own. |
| PWM driver | **2× PCA9685** on the header's I2C (SDA `GPIO7` pin 3, SCL `GPIO8` pin 5) — `0x40` front axle, `0x60` rear |
| Motor driver | 4× BTS7960 full H-bridge |
| Camera | MIPI-CSI, 2-lane, connector `J4`; SCCB shares the header's I2C 0 with both PCA9685 boards (`i2c_bus.c` owns the bus, `BOARD_SCCB_HZ` on the camera, 400 kHz on the PWM boards); sensor **OV5647** (Aitewin 5MP night-vision fisheye), confirmed on the bench 2026-09-14; no reset/pwdn pin wired. `esp_ipa`'s IDF-6 archive is built for revision ≥3.0 (Zba/Zbb) and panics on this chip — `firmware/car/core/CMakeLists.txt` links its IDF-5.5 archive instead whenever the <3.0 family is selected |
| Framework | ESP-IDF **6.0.2** at `~/esp/esp-idf-v6.0.2` |

**The C6 is a modem, not a brain.** It runs Espressif's `esp_hosted` slave image — a vendor
artifact we pin, never author. Application code calls the ordinary `esp_wifi` API and
`esp_wifi_remote` marshals it over SDIO, so `wifi_ap.c` is chip-agnostic. The radio's image is
built into the car's own firmware and delivered over SDIO by the car itself: a mismatch at boot
makes the car push the embedded image at the C6 and restart, so one OTA updates both processors
and an `esp_hosted` pin bump no longer means a bench visit
(`docs/superpowers/specs/2026-08-31-radio-in-one-image-design.md`). The UART header remains the
recovery path, and `firmware/car/modem/flash-radio.sh` still builds the image standalone.

### Motor channel mapping (sequential, stride 2)

Channel numbers here are **logical**: the firmware speaks 0..7 throughout, and `pca9685.c` is
the only file that knows they live on two boards (0..3 on the front board, 4..7 on the rear).

| Motor | CH_A (forward) | CH_B (reverse) | Board |
|---|---|---|---|
| 1 | CH0 | CH1 | front (`0x40`) |
| 2 | CH2 | CH3 | front (`0x40`) |
| 3 | CH4 | CH5 | rear (`0x60`) |
| 4 | CH6 | CH7 | rear (`0x60`) |

Which pair drives which *corner* is not fixed here — the calibration wizard discovers it by
spinning each pair and asking which wheel turned, so a swapped cable costs nothing.

Never both HIGH — that is shoot-through on a BTS7960. `motors_plan` makes it structurally
impossible: per wheel it sets exactly one of the pair nonzero, or neither.

## Layout

```
app/                 iOS pult (XcodeGen; the .xcodeproj is generated and gitignored)
firmware/
  car/core/          the car's firmware — all logic
  car/modem/         the radio's slave image build
  dongle/            the USB-Ethernet dongle — knows nothing about the car, and holds the
                     car's network in RAM only: the app tells it on every launch
tools/               mock_car, release.sh, env-p4.sh
docs/                protocol.md, bringup.md, specs/, plans/, research/
```

`app/` and `firmware/car/core/` **do not reference each other**. Their only seam is
`docs/protocol.md` (the wire contract) and `tools/mock_car` (an executable stand-in for the
car). If a change makes one need to know about the other, the change is wrong.

`firmware/car/modem/` sits under `car/` because it is the car's second processor, not
because it knows anything about the car — it knows neither the motors nor the protocol.

## The contract

`contract/car-api.json` is the source of truth for everything both sides agree on: the
protocol version, the real-time channel's constants, the video channel's own section (port,
the 12-byte wire header, timing, the header vectors all three receivers are tested against),
the six status groups (`link`, `motors`, `radio`, `storage`, `system`, `video`) and the frozen
`/version` document (`device`, `fw`, `build`, `proto`, `rolled_back`) both boards serve, the
state words each group's enum fields take (`motors.owner`, `motors.bus`,
`radio.state`, `video.state`), the six config domains with their ranges and defaults, and the
car's error codes. `tools/gen_contract.py` emits all four expressions of it — the firmware's
descriptor table (`main/cfg_table.inc`, plus the key, type-word and error-code macros the
printers in `telemetry.h`, `device_json.h`, `status_api.c` and `calib_api.c` build their
format strings from), the app's Swift structs (`app/AJMiddleCar/Generated/CarAPI.swift`, which
now includes the generated `Telemetry` and `CarStatus` structs alongside the config ones), the
mock's table and validator (`tools/mock_car/generated.py`), and the endpoint table inside
`docs/protocol.md`. The dongle's side of the same idea is `contract/dongle-api.json` and
`tools/gen_dongle.py`, now carrying `relay.video_port`, `relay.video_max_kbps` and the three
nullable `relay.video_*` status fields alongside the real-time ones.

Never hand-edit a generated file. Change the schema and re-run the generator;
`tools/check_contract.sh` fails a tree where the two disagree, and `tools/test-all.sh`
runs it alongside the tests.

## Firmware architecture

The pure modules have **zero ESP-IDF dependencies** and are host-tested with plain `cc`.

- `board.h` — **every** assumption about the physical board: I2C pins, bus speed, PWM frequency,
  the radio's delivery route (its expected version is derived from the esp_hosted component).
  Bring-up edits this file and nothing else.
- `identity.h` — which car this is: `CAR_DEVICE_ID`, SSID, password. Distinct from `board.h`,
  which is about which board it runs on.
- `mixer.{c,h}` — *pure*. Tank-turn mixing: `left = t+y`, `right = t−y`, normalised to keep
  `[-1,1]` while preserving the turn ratio.
- `motors.{c,h}` — *pure*. Side speeds → 8 PWM duties through a per-wheel calibration table.
  Shoot-through-safe by construction.
- `control_proto.{c,h}` — *pure*, zero-alloc parser for the 10 Hz control frame. Deliberately
  not cJSON: ten parses a second is ten mallocs a second on the control path. Datagrams are
  typed — `hello`, `drive`, `bye`, `view` — read from the wire's `type` key, not guessed from
  which other keys showed up. `view` (the video channel's subscription, below) is a `hello`
  shape with an optional `key` flag, so it costs this parser nothing new.
- `car.{c,h}` — clamps, mixes, plans, and offers the duties to the actuator arbiter. Holds the
  mutex around the calibration read, with a bounded 200 ms wait so a stuck holder cannot wedge
  the watchdog.
- `ramp.{c,h}` — *pure* slew step plus the `ramp` domain of `/config`; the 50 Hz actuator task
  lives in `link.c`. Bounded rise, instant fall.
- `link.{c,h}` — the actuator arbiter (who may command the motors: `rt`, `console`, `calib`,
  `recover`, `ota`, `safe`) and the 50 Hz task that is the **sole writer** to the PCA9685.
- `rt_link.{c,h}` — the UDP real-time channel: session ownership, the sequence gate, the
  control watchdog (300 ms without a command calls `recovery_on_link_lost()`), and the 5 Hz
  telemetry push. `watchdog.h` keeps only the pure staleness predicate. `rt_link_owner_sid()`
  hands the session owner's sid to the video channel, copied under a critical section — the
  sid is 16 bytes the rt task rewrites on every adoption, and video's reader runs three
  priorities below it.
- `recovery.{c,h}` — breadcrumb ring buffer; on link loss a task replays it reversed and negated
  to retrace back into range, aborting the instant a frame arrives.
- `i2c_bus.{c,h}` — owns the one I2C master on the header's SDA/SCL (`GPIO7`/`GPIO8`): created
  once at boot, and every device on the wire adds itself to this handle rather than opening a
  bus of its own, since the driver refuses a second master on one port. Speed is a per-device
  property (`scl_speed_hz`), so the PWM boards stay at 400 kHz while the camera's SCCB runs
  `BOARD_SCCB_HZ` on the same wire. `pca9685.c` used to create the bus itself; it now asks this
  module for the handle, same as the camera does.
- `camera.{c,h}` — the sensor and the capture pipeline, behind `esp_video` **2.4.1**,
  `esp_cam_sensor` **2.4.0** and `esp_ipa` **2.3.0** (its AE/AWB, a closed prebuilt library —
  pinned, never patched, like the radio's image). `esp_video` 2.4.1's own manifest wants
  `esp_cam_sensor` 2.4.\* and `esp_h264` 1.3.\*, not the newer specs an earlier draft of the
  design assumed. `camera_init` detects the sensor once at boot (no answer is `off`, not a
  boot failure — the car still drives); the pipeline itself runs only between `camera_start`
  and `camera_stop`, which is what lets a firmware update ignore the camera's existence
  entirely. `camera_acquire` is bounded, not blocking: `camera_start` sets
  `VIDIOC_S_DQBUF_TIMEOUT` to 500 ms, because `esp_video`'s VFS has no `select()` — a sensor
  that stops delivering frames ends the caller's request instead of wedging its task.
- `video_enc.{c,h}` — the hardware H.264 encoder, `esp_h264` **1.3.8**, driven directly rather
  than through its V4L2 device: `force_idr()` — the whole recovery story — is not reachable
  through `/dev/video11`. A wide QP corridor, not `esp_video`'s narrow default, is what makes
  `bitrate_kbps` mean anything.
- `video_wire.h` — *pure*: the 12-byte wire header, chunking, and the nine reassembly rules in
  `docs/protocol.md`'s video section — the same vectors and the same verdicts as
  `VideoWire.swift` and `video_wire.py`. The receiver is a bitset over chunk indices, never a
  counter, so a duplicate cannot fake a finished frame.
- `video_sub.h` — *pure*, header-only: the subscription as arithmetic — who may watch (the
  real-time session's owner, by sid), for how long after the last `view`, and how often
  `key:true` may force an IDR.
- `video_link.{c,h}` — the video channel: one UDP socket on `4211`, three tasks below the
  actuator — control (the socket's receive side and the subscription), encode (camera →
  `video_enc` → a two-slot ring, handed off with a release/acquire store rather than a lock),
  and a sender that drains the ring one chunk every 3 ms off an `esp_timer` (the dongle's USB drains ~4 Mbit/s — bringup.md), so a keyframe
  leaves as a trickle rather than a burst. Nothing here touches the motors.
- `video_cfg.{c,h}` — the `video` domain of `/config`: the encoder's target bitrate, read once
  at stream start rather than mid-frame.
- `snapshot_api.c` — `GET /snapshot`: one JPEG of whatever the camera sees, for the bench, no
  app or subscription involved. `409` while the stream owns the pipeline (this silicon's
  hardware JPEG block cannot take the stream's YUV420 buffers), `500` when there is no sensor
  to ask.
- `pca9685`, `wifi_ap`, `http_server`, `telemetry`, `calibration`, `wheel`, `dims`,
  `trim`, `cfg_json` and the four `*_api` modules — driver, transport, config, persistence.
  `cfg_api.c` serves one route, `/config`, for all six domains — GET walks every domain,
  POST validates whatever subset it was sent before applying any of it. `calib_api.c` speaks
  corners by name (`front_left`, `front_right`, `rear_left`, `rear_right`) and `inverted`
  rather than array position and sign; `calibration.c` itself, and what NVS stores, are
  unchanged underneath it.

All configuration persists in NVS as **one JSON string per domain**, with a dirty check so an
unchanged POST does not rewrite flash.

`esp_video`'s `isp_task` outranks everything above — see Gotchas below.

On the dongle (`firmware/dongle/main/`), `relay_udp.c` is the same ~450 lines run **twice**
rather than copied: port, task name, priority and counters are parameters
(`relay_udp_cfg_t`), so the real-time channel (`4210`, priority 5) and video (`4211`, priority
4) share one implementation. Video's instance is admitted toward the phone by
`rate_gate.{c,h}` — *pure*, host-tested: bytes per fixed window (**1000 ms**), charged against
`relay.video_max_kbps`; a datagram over budget is refused and counted
(`relay.video_dropped`) here, where it is visible, rather than silently in `esp_tinyusb` when
the NTB pool fills up. The real-time instance is never throttled.

## Build

```bash
source tools/env-p4.sh          # ESP-IDF 6.0.2; the 5.4 install AJPicoCar uses is untouched
cd firmware/car/core && idf.py build
idf.py -p /dev/cu.usbmodem* flash monitor
```

The USB port number changes after every reset — re-check with `ls /dev/cu.usbmodem*`.

**Host tests** — everything that runs without hardware, a simulator or ESP-IDF:

```bash
tools/test-all.sh
```

That covers the contract (schema, generator, drift), the firmware's pure modules, the app's
pure Swift, and — against a mock it starts and stops itself — the REST and real-time
conformance sweeps plus `tools/conformance_video.py`, which subscribes on the video port,
reassembles chunks by the same rules as the car and the app, and checks them against the
contract's `video.vectors`. `make -C firmware/car/core/test run` still works on its own for
the C half.

**Radio image** (rare): `firmware/car/modem/flash-radio.sh` builds it; `firmware/car/modem/README.md` covers both
ways to get it onto the C6 — over SDIO from the host, or over its UART header.

## iOS app

SwiftUI, XcodeGen, landscape-locked, Russian-localised, warm light/dark themes.

Запуск — это лестница плат: массив `[адаптер, машинка]` (или `[машинка]` без адаптера) и одна
стадия `reach → /version → выпуск → правило → шаг` (`StageRule`, чистая, хост-тест
`app/tests/stagerule`). До машинки добираются через адаптер — `CarReach` (`app/tests/carreach`),
сетевой автомат с бюджетом попыток. `AppFlow` — проводка: фазы `Phase.stage(device, GateStep)`,
раннер `runLadder`/`restart`/`updateFinished`. Стражи связи после гейта (провод пропал, чужой
hello, чужой proto) не рисуют своих экранов, а перезапускают лестницу с нужной ступени
(`restart(from:)`). Один экран на всё — `ConnectView(.stage(device, step))`; `WrongCarView`
больше нет.

```bash
cd app && xcodegen generate
xcodebuild build -scheme AJMiddleCar -destination 'platform=iOS Simulator,name=iPhone 17' -derivedDataPath /tmp/ddata-middle
```

Hardware-free loop: run the mock car and the simulator points at it automatically.

```bash
cd tools/mock_car && python3 -m venv .venv && .venv/bin/pip install -r requirements.txt
nohup .venv/bin/python -u mock_car.py >/tmp/mock.log 2>&1 &
```

The mock's video port loops `tools/mock_car/video.py` over `tools/mock_car/sample.h264`
(Annex B, checked in, ≤400 KB) — `--video-loss-pct`, `--video-reorder-pct` and
`--video-dup-pct` impair it the same way `--loss-pct` impairs the real-time channel, and
`tools/test-all.sh` runs `tools/conformance_video.py` against it at 0.3% loss.

`CarHost` is the single source of the address: `127.0.0.1:8080` in the simulator, the dongle's
`192.168.7.1` on a device (REST `:80`, UDP `:4210` and UDP `:4211` relayed to the car unchanged;
the dongle's own API on `:8080`). **Bench without a phone:** plug the dongle into the Mac (it
appears as a USB-Ethernet interface, the Mac gets `192.168.7.2`) and launch the simulator with
`-viaDongle` — it then IS the phone: same address, same relay ports, the whole launch ladder
including the adapter's own update, the car's forced update and the drive screen with video.
`xcrun simctl launch booted com.adamjohnson.ajmiddlecar -viaDongle`, screenshot with
`xcrun simctl io booted screenshot`. This is how the first FPV bench round was run (2026-09-14). There is no direct path from a device to the car and no
argument that opens one — the bench escape hatch was retired 2026-09-13. `MOCK_DEVICE=esp32-car`
makes the mock impersonate the other car, which is how the wrong-car screen is exercised;
`--no-version` (or `MOCK_NO_VERSION=1`) makes it answer 404 on `/version` until the first
accepted OTA — the flag-day rehearsal, a car older than the endpoint.

`CarLink.video` (a `VideoLink`) and `VideoView` add the FPV picture to the drive screen: a
`view` subscription over `CarHost.videoPort`, tied to the same session `CarLink` opens, feeding
an `AVSampleBufferDisplayLayer` frame by frame. Reassembly runs on `VideoLink`'s own queue, and
`onFrame` is confined there — never called from the main actor, which only reads the published
counters back across that same queue.

The drive screen is a HUD: the picture is a 16:9 window onto the 4:3 frame (`resizeAspectFill`,
the fisheye's top and bottom eighths cropped), and every instrument keeps to its edges — nothing
sits in the middle of the picture with a scrim behind it. `DriveLayout` (pure, host-tested) is
where the pieces go, derived from the screen and its safe area, not from one model's numbers;
`docs/superpowers/specs/2026-09-15-drive-hud-design.md` says why each piece is where it is.

Pure Swift modules are host-tested with `swiftc` directly — no XCTest runtime needed.

## Gotchas

1. **`mix` on the console is exempt from the watchdog** — bench debugging does not stop every
   300 ms. Only UDP command datagrams on `rt_link` feed it.
2. **"Motors don't spin" is usually delivery, not firmware.** Opening the serial port resets the
   board, so a command sent in the first second is swallowed during boot. And a control client
   must *stream* the held command at 10 Hz: one frame is a ~40 ms pulse that cannot visibly move
   a motor. Confirm the `drive ...` log echoed before believing anything is broken.
3. **A dropped link makes the car reverse, not stop.** That is `recovery` retracing its path. A
   client that stops streaming mid-drive will see it.
4. **All grounds common** — board, PCA9685, BTS7960, battery negative.
5. **BTS7960 needs R_EN + L_EN tied HIGH.** Without it the bridge is electrically disconnected:
   PWM exists, motors stay silent.
6. **The app is landscape-locked and every split screen draws its own header** via `SplitScreen`.
   Never add a system `.navigationTitle` to one — it reintroduces the inset that shifts content.
7. **Simulator screenshots come out rotated 90°** (landscape app, portrait window) — fix with
   `sips -r -90`. You cannot tap from the CLI, so address a gallery frame directly:
   `--args -gallery -galleryIndex N`. It also hides the gallery's own caption, so the capture is
   only what a user would see. One build, then one launch per frame; the old advice here was to
   re-seed a `@State` default, which costs a full rebuild per screen.
8. **Free Apple-ID signing expires every 7 days.** Re-run from Xcode. There is no web pult.
9. **Both Type-C ports go to the P4, not to the C6.** esptool reports the same MAC on each; one is
   the native USB (`usbmodem*`), the other the CH343P bridge on UART0 (`wchusbserial*`). The C6 has
   its own UART header — but it can also be reflashed over SDIO with no wire at all
   (`firmware/car/modem/README.md`).
10. **The radio's version is load-bearing, not cosmetic.** A mismatch costs five seconds of every
    boot (a timed-out RPC), disables SDIO aggregation, and leaves `radio.state` at `mismatch`
    instead of `ok`.
11. **`esp_video`'s `isp_task` runs at priority 11 — above the actuator (5) and `rt_link` (6),
    above everything of ours.** Fixed in the component, not a knob in `board.h`. It is created
    once at boot (`camera_init`) and parks on an empty statistics queue between streams rather
    than being torn down, so it only competes for CPU while `streaming`; each wake is AE/AWB
    plus one SCCB write, short by design, but whether it is short enough to stay invisible in
    `link.rx_hz` and actuator jitter **with the pipeline actually running is an open bench
    question**, not yet answered (`docs/bringup.md`). If it measures otherwise, the fix is an
    `esp_video` override with a patched priority, not a car-side workaround.

## Status

Ported from AJPicoCar with feature parity, and **first run on hardware 2026-08-20**: the board
boots, the softAP comes up, and the radio was updated from its shipped image to the pinned 3.0.6.
The motors were not wired at first, so `board.h`'s I2C pins went unverified for a while — a
stock build boots anyway with `motors.bus:"down"` (network and OTA up, motors inert, by design).
`docs/bringup.md` is the live record of what the board has and has not answered.

Two notes that used to live here are stale as of 2026-08-31 and were corrected on the bench:
the native USB port **does** enumerate (esptool detects the P4 on `/dev/cu.usbmodem*` and
flashes over it), and the console on UART0 is not a local override — it is committed policy in
`firmware/car/core/sdkconfig.defaults`, with the reason written at that line: a board whose native USB
goes silent would otherwise have no console at all.
