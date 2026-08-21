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
| Framework | ESP-IDF **6.0.2** at `~/esp/esp-idf-v6.0.2` |

**The C6 is a modem, not a brain.** It runs Espressif's `esp_hosted` slave image — a vendor
artifact we pin, never author. Application code calls the ordinary `esp_wifi` API and
`esp_wifi_remote` marshals it over SDIO, so `wifi_ap.c` is chip-agnostic. The radio's image is
built by `firmware/c6/flash-radio.sh` and can be delivered either over the C6's UART header or —
as was actually done — over the SDIO link itself, from the host; `/status` reports its version so
a pinned-version mismatch is visible rather than mysterious.

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
app/            iOS pult (XcodeGen; the .xcodeproj is generated and gitignored)
firmware/p4/    the car's firmware — all logic
firmware/c6/    the radio's slave image build
tools/          mock_car, release.sh, env-p4.sh
docs/           protocol.md, bringup.md, specs/, plans/, research/
```

`app/` and `firmware/p4/` **do not reference each other**. Their only seam is
`docs/protocol.md` (the wire contract) and `tools/mock_car` (an executable stand-in for the
car). If a change makes one need to know about the other, the change is wrong.

`firmware/c6/` knows nothing about the car at all — not the motors, not the protocol.

## The contract

`contract/car-api.json` is the source of truth for everything both sides agree on: the
protocol version, the real-time channel's constants, and the five config domains with
their ranges and defaults. `tools/gen_contract.py` emits all four expressions of it —
the firmware's descriptor table (`main/cfg_table.inc`), the app's Swift structs
(`app/AJMiddleCar/Generated/CarAPI.swift`), the mock's table and validator
(`tools/mock_car/generated.py`), and the endpoint table inside `docs/protocol.md`.

Never hand-edit a generated file. Change the schema and re-run the generator;
`tools/check_contract.sh` fails a tree where the two disagree, and `tools/test-all.sh`
runs it alongside the tests.

## Firmware architecture

The pure modules have **zero ESP-IDF dependencies** and are host-tested with plain `cc`.

- `board.h` — **every** assumption about the physical board: I2C pins, bus speed, PWM frequency,
  expected radio version. Bring-up edits this file and nothing else.
- `identity.h` — which car this is: `CAR_DEVICE_ID`, SSID, password. Distinct from `board.h`,
  which is about which board it runs on.
- `mixer.{c,h}` — *pure*. Tank-turn mixing: `left = t+y`, `right = t−y`, normalised to keep
  `[-1,1]` while preserving the turn ratio.
- `motors.{c,h}` — *pure*. Side speeds → 8 PWM duties through a per-wheel calibration table.
  Shoot-through-safe by construction.
- `control_proto.{c,h}` — *pure*, zero-alloc parser for the 10 Hz control frame. Deliberately
  not cJSON: ten parses a second is ten mallocs a second on the control path.
- `car.{c,h}` — clamps, mixes, plans, and hands the duties to the ramp task. Holds the mutex
  around the calibration read, with a bounded 200 ms wait so a stuck holder cannot wedge the
  watchdog.
- `ramp.{c,h}` — 50 Hz task, the **sole writer** to the PCA9685. Bounded rise, instant fall.
- `watchdog.{c,h}` — 50 Hz check; 300 ms without a control frame calls `recovery_on_link_lost()`.
- `recovery.{c,h}` — breadcrumb ring buffer; on link loss a task replays it reversed and negated
  to retrace back into range, aborting the instant a frame arrives.
- `pca9685`, `wifi_ap`, `http_server`, `ws_control`, `telemetry`, `calibration`, `wheel`, `dims`,
  `trim`, `cfg_json` and the seven `*_api` modules — driver, transport, config, persistence.

All configuration persists in NVS as **one JSON string per domain**, with a dirty check so an
unchanged POST does not rewrite flash.

## Build

```bash
source tools/env-p4.sh          # ESP-IDF 6.0.2; the 5.4 install AJPicoCar uses is untouched
cd firmware/p4 && idf.py build
idf.py -p /dev/cu.usbmodem* flash monitor
```

The USB port number changes after every reset — re-check with `ls /dev/cu.usbmodem*`.

**Host tests** — everything that runs without hardware, a simulator or ESP-IDF:

```bash
tools/test-all.sh
```

That covers the contract (schema, generator, drift), the firmware's pure modules and the
app's pure Swift. `make -C firmware/p4/test run` still works on its own for the C half.

**Radio image** (rare): `firmware/c6/flash-radio.sh` builds it; `firmware/c6/README.md` covers both
ways to get it onto the C6 — over SDIO from the host, or over its UART header.

## iOS app

SwiftUI, XcodeGen, landscape-locked, Russian-localised, warm light/dark themes.

```bash
cd app && xcodegen generate
xcodebuild build -scheme AJMiddleCar -destination 'platform=iOS Simulator,name=iPhone 17' -derivedDataPath /tmp/ddata-middle
```

Hardware-free loop: run the mock car and the simulator points at it automatically.

```bash
cd tools/mock_car && python3 -m venv .venv && .venv/bin/pip install -r requirements.txt
nohup .venv/bin/python -u mock_car.py >/tmp/mock.log 2>&1 &
```

`CarHost` is the single source of the address: `127.0.0.1:8080` in the simulator,
`192.168.4.1` on a device. `MOCK_DEVICE=esp32-car` makes the mock impersonate the other car,
which is how the wrong-car screen is exercised.

Pure Swift modules are host-tested with `swiftc` directly — no XCTest runtime needed.

## Gotchas

1. **`mix` on the console is exempt from the watchdog** — bench debugging does not stop every
   300 ms. Only `/ws` traffic feeds it.
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
7. **Simulator screenshots come out rotated 90°** (landscape app, portrait window), and you
   cannot tap from the CLI — drive the debug gallery (`--args -gallery`) via a temporary
   `@State index` seed instead.
8. **Free Apple-ID signing expires every 7 days.** Re-run from Xcode. There is no web pult.
9. **Both Type-C ports go to the P4, not to the C6.** esptool reports the same MAC on each; one is
   the native USB (`usbmodem*`), the other the CH343P bridge on UART0 (`wchusbserial*`). The C6 has
   its own UART header — but it can also be reflashed over SDIO with no wire at all
   (`firmware/c6/README.md`).
10. **The radio's version is load-bearing, not cosmetic.** A mismatch costs five seconds of every
    boot (a timed-out RPC), disables SDIO aggregation, and leaves `radio.ok` false.

## Status

Ported from AJPicoCar with feature parity, and **first run on hardware 2026-08-20**: the board
boots, the softAP comes up, and the radio was updated from its shipped image to the pinned 3.0.6.
Two things are known and unfinished — the motors are not wired yet, so `board.h`'s I2C pins are
still unverified and a stock build aborts in `pca9685_init` with nothing on the bus; and the
native USB port stopped enumerating, so bench work runs with a local override putting the console
on UART0. Both are written up in `docs/bringup.md`, which is the live record of what the board
has and has not answered.
