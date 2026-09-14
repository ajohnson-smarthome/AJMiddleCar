# Bring-up checklist

This project was built with the board on order rather than on the bench — a deliberate choice,
which is why every assumption about the hardware was quarantined instead of scattered. Three of
them are settled by building; the rest wait here.

Fill the answers in as you go. If an answer differs from what the code assumes, the fix is named
next to it.

## Open assumptions

Four of the six were settled at the desk, exactly as the spec predicted — by building, with no
board present. The board arrived on 2026-08-20 and both remaining assumptions were answered within
a day — the radio that evening, the I2C pins once the PCA9685 boards were wired. **All six are now
closed.**

| # | Assumption | Status |
|---|---|---|
| 3 | `esp_wifi_remote` proxies `esp_wifi_ap_get_sta_list` | **Resolved — it links.** RSSI telemetry survives the port |
| 4 | An explicit hosted transport-init call in `app_main` | **Resolved — none needed.** `ESP_HOSTED_AUTO_CALL_INIT_BEFORE_APP_MAIN` brings the transport up before `app_main` |
| 5 | 5.4-era code builds unchanged on IDF 6.0.2 | **Resolved — one break.** cJSON left ESP-IDF for the component manager; declared in `idf_component.yml`. Nothing else needed changing |
| 6 | The `esp_hosted` slave builds as a project we own | **Resolved — it is theirs, and it builds.** A standard IDF project inside the pinned component; `firmware/car/modem/` holds only the procedure. Needs a vendor patch to ESP-IDF (`eh.py patch-idf`), applied automatically |
| 2 | The C6 arrives already flashed with the slave image | **Resolved — flashed, but not with our version.** It shipped an image reporting `0.0.0`, four major versions behind the pinned 3.0.6. WiFi worked anyway; see the bench notes for how it was updated |
| 1 | The I2C pins in `board.h` are free and clear of the SDIO link | **Resolved — but not at the pins the code first guessed.** The header's I2C is SDA `GPIO7` / SCL `GPIO8`; both boards answer and initialise there |

**What is now known about the pins.** SDIO reserves **GPIO 14–19** on the P4 (D0–D3, CLK, CMD)
plus **GPIO 54** for co-processor reset, pinned in `firmware/car/core/sdkconfig.defaults`. Neither
appears on the 2×20 header except GPIO 54, which sits on physical pin 32 — leave that one alone or
the radio resets.

`board.h` now puts I2C on **GPIO 7 and 8**, which is the header's own I2C pair: physical pins 3 and
5, silkscreened `SDA`/`SCL` rather than by number, in the Raspberry Pi positions. The numbers that
stood here before, GPIO 20 and 21, are also on the header (pins 13 and 11) and would have worked —
but they were a guess, and the labelled pair is the one to use.

The second question this section used to ask — whether Waveshare wires the C6 to the GPIOs the
component defaults to — answered itself: the radio comes up, so they do.

**Fixes if an answer is no:**

1. See above.
2. Answered, and the fix turned out not to need the wire the README assumed — the co-processor
   was updated over SDIO from the host. `firmware/car/modem/README.md` now carries both procedures.
3. Already resolved; no action.

### The camera — open, from `docs/superpowers/specs/2026-09-14-fpv-video-design.md`

Five more, none answerable by building — the same desk/bench split as the six above, and
stage 1 of the video plan exists to close them.

| # | Assumption | Status |
|---|---|---|
| 7 | The sensor is **OV5647** (SCCB `0x36`, chip id `0x5647`) — every 5 MP fisheye/night-vision Pi camera module is | **closed 2026-09-14** — the first boot with the ribbon in logged `ov5647: Detected Camera sensor PID=0x5647` and `camera: sensor up on /dev/video0, 1280x960`, over the shared bus at 100 kHz |
| 8 | Whether this particular module is **NoIR** (no IR-cut filter — daytime colours skew pink, a property, not a defect) or carries a mechanical IR-cut plus a photoresistor fed from 3.3 V | **open** — inspect the lens board; a photoresistor changes the current budget too |
| 9 | A snapshot shows the **whole lens's field of view**, not a crop — `esp_video` does not scale on this silicon, and crops only on silicon ≥ v3.0, which this board is not | **open** — check `GET /snapshot` against what the lens actually sees, right side up |
| 10 | The bus survives 400 kHz SCCB on top of the two PCA9685 pull-ups, the board's own 2.2 kΩ, and a camera ribbon | **open** — the firmware ships conservatively at **100 kHz** (`BOARD_SCCB_HZ`, `board.h`) until the scope says the edges pass at 400 kHz; raise it only then |
| 11 | Time from `camera_start` to the first usable frame | **open** — budgeted at ≈0.2–0.3 s (sensor lock-in plus AE/AWB convergence), unmeasured on hardware. Bounded regardless: `camera_start` sets `VIDIOC_S_DQBUF_TIMEOUT` to 500 ms, so a sensor that never delivers a frame ends the caller's request instead of hanging its task |

`GET /snapshot` is stage 1's own test rig for #7–#9: the JPEG it returns is the fastest way to
see the sensor, its filter and its field of view without an app or a wire in the way.

## Bench sequence

- [x] **Work out the two Type-C ports.** Both lead to the **same chip** — esptool reports the
      identical MAC `e8:f6:0a:e0:ae:26` on either — so they are two ways into the P4, not two
      chips. Neither reaches the C6.

      | Port | Enumerates as | Flash? | Console? |
      |---|---|---|---|
      | native USB | `/dev/cu.usbmodem*` — Espressif "USB JTAG_serial debug unit", VID `0x303A` | yes | yes, as built (USB-Serial-JTAG) |
      | CH343P bridge | `/dev/cu.wchusbserial*` — "USB Single Serial", VID `0x1A86` PID `0x55D3` | yes, over the P4's UART0 | only if the console is built for UART0 |

      Which physical connector is which is not recorded here, because the native USB stopped
      enumerating partway through the first session and never came back — see the bench notes.
      The bridge became the working channel, and the sanctioned fix applied: the console moved
      rather than the cable.
- [x] **Flash.** `cd firmware/car/core && source ../../tools/env-p4.sh && idf.py -p /dev/cu.usbmodem* flash monitor`
      — works on either port. Needed one config change first; see the bench notes on chip revision.
- [x] **Radio.** `esp-hosted fw versions: host=3.0.6 coprocessor=3.0.6 (match)`, and `status_api`
      logs `radio firmware 3.0.6`, which it only does when `radio.ok` is true.
- [x] **SDIO pull-ups.** Present. Not inspected visually — inferred from behaviour, which is
      stronger: the link comes up as `SDIO 4-bit 40000 kHz` and reports `Card init success`,
      which is exactly what missing `D2`/`D3` pull-ups would prevent by dropping the slave to SPI.
- [x] **Network.** SSID `AJMiddleCar` appears; the dongle joins it and gets an address on `192.168.4.x`
      (`joined: ip=192.168.4.2 gw=192.168.4.1`, 2026-08-31 — see `firmware/dongle/README.md`). The
      phone itself never joins: it reaches the car through the dongle.
- [x] **Identity.** `curl http://192.168.7.1/status` through the relay returns `"device":"ajmiddlecar"`
      (2026-08-31, same table).
- [x] **I2C.** Both boards answer and initialise — `0x40` front, `0x60` rear. The rear address
      is not the `0x41` this file originally assumed; see the bench notes. A bus scan is the
      quickest way to check, and it is self-verifying because the on-board ES8311 codec sits at
      `0x18`: if `0x18` answers and the PCA9685s do not, the pins are right and the fault is in
      their wiring or power.
- [x] **One wheel.** Better than one: `mix 0.3 0` turned the motors on the first try, and
      `mix 1 0` held full duty (4095) for ten seconds without complaint. Note the console is not
      watchdog-backed, so a console `mix` runs until `mix 0 0` — send the stop from the same script
      that sends the drive, and make it insistent, because a lost link would otherwise leave the
      motors at full until a power cycle.
- [ ] **Calibration.** Run the app's wizard end to end; `/status` reports `calibrated: true`,
      and it survives a power cycle.
- [ ] **Driving.** Both schemes, forward, reverse, tank turn.
- [ ] **Watchdog.** Drive, then drop WiFi mid-drive. The car must retrace and stop, and
      `wdt_trips` must increment. This has never been tested on the pico car either — it is the
      oldest untested behaviour in the family.
- [ ] **OTA.** Cut a release, let the app's launch gate force-update the board, confirm it boots
      and `fw` reports the new build.

### Video — after `docs/superpowers/specs/2026-09-14-fpv-video-design.md`, once its own stage 4 is done

The order follows the plan's own stages: a snapshot first (stage 1), then a stream to a Mac
sitting directly on the car's own Wi-Fi with `tools/conformance_video.py` (stage 2 — not
repeated here, see the plan), then through the dongle measuring `relay.video_kbps` /
`relay.video_dropped` at 1.5 / 2 / 2.5 Mbit/s (stage 3), then the app (stage 4). These eight
are the closing sweep, run once stage 4 itself passes.

- [ ] **Snapshot.** `video.state:"idle"`; `GET /snapshot` returns a JPEG showing the whole
      lens's field of view, right side up.
- [ ] **Motors alongside the camera.** `motors.bus:"ok"` with the camera on the shared I2C
      bus; calibration still spins the wheels as before. Attach the SCL scope trace here (see
      Open assumptions, #10, above).
- [ ] **Stream through the wire, 60 s at 1.5 Mbit/s.** `tools/conformance_video.py`'s measured
      loss < 0.5%, `relay.video_dropped` does not grow, `link.rx_hz` holds 10, `link.timeouts`
      does not grow — all with the capture pipeline actually running.
- [ ] **Stopwatch in frame.** Ordinary frames arrive ≤ 150 ms after capture; after an induced
      loss (the mock's `--video-loss-pct`) and a real one (a hand on the antenna), the picture
      returns ≤ 250 ms.
- [ ] **Leaving the drive screen.** `video.state` returns to `idle` within ≤ 3 s of leaving the
      screen, and on the same tick for `bye`; backgrounding the app and returning brings the
      picture back with no restart.
- [ ] **Firmware update with the drive screen open.** Confirm it cannot start from that screen
      at all, and that the capture pipeline is stopped before any flash begins.
- [ ] **Camera disconnected.** The car boots and drives normally with `video.state:"off"`; the
      drive screen shows the no-picture indicator.
- [ ] **Stacks and PSRAM.** High-water marks for the encode task, `isp_task`, and both
      `relay_udp` instances in the log; PSRAM free ≥ 20 MB while `streaming`.

## Notes from the bench

_Record anything surprising here — it is the raw material for the next spec._

### The dongle kept an association the car had forgotten (2026-09-14)

First over-the-air update of the car through the dongle, watched from a Mac on the dongle's
USB: the upload completed, the car rebooted into v1.0+864 (`rolled_back:false`), and from then
on the relay could not reach it. `wifi.state` stayed `connected`, RSSI kept updating from the
beacons — and `relay.last_error` counted `errno 12 Not enough space` on every send toward the
car: the softAP had restarted and forgotten this station, no disconnect event ever came, and
the radio's transmit buffers filled with frames nobody acknowledged. `POST /wifi` with the
same network was a no-op by design while the station said `connected`, so the app's
«Повторить» could not have helped either; only a replug would have. Handing the dongle a
bogus network and then the real one — a genuine reassociation — brought the relay back
instantly, and the simulator was driving with live video within seconds.

It happened again an hour later, on the next update, with one difference that mattered: no
errors at all. With only hello at 5 Hz toward the car — no video — the transmit buffers never
filled, every send *succeeded*, and the frames simply vanished after their retries. So the
first version of the fix, which counted failed sends, saw nothing.

Fix: `uplink.{c,h}` scores silence, not failure — every send toward the car is a strike
whether it went out or not, and anything heard from the car (a datagram on either relay, bytes
on a TCP slot, a SYN answered) clears the count. Twenty strikes while the station still says
`connected` ask it to re-join (`wifi_sta_rejoin`), no more than once per ten seconds; an
unchanged `POST /wifi` on that same state re-joins too. A car reboots on every update, so this
would have hit every OTA from now on.

Also from the same session: the snapshot came out black (mean 1.8/255) because ten frames is
~220 ms and AE needs ~2.7 s from cold — the warm-up is now three seconds by the clock. And the
picture itself: the car encodes at 1842 kbit/s, the dongle relays 1322 — about a quarter of
the bytes vanish between the car's `sendto` and the dongle's `recvfrom`, with `video_dropped`
at 0 on both sides, so 10–14 % of frames are lost somewhere no counter looks. Open.

### The AE/AWB library was built for silicon this board is not (2026-09-14)

The first boot of v1.0+862 with the camera ribbon in detected the sensor and panicked in the
same millisecond: `Guru Meditation Error: Core 0 panic'ed (Illegal instruction)` at
`esp_ipa_pipeline_create`, seven boots in thirty seconds. `MTVAL` held the instruction,
`0x20fac7b3` — `sh2add`, from the Zba extension. `readelf -A` on the archive says why:
`esp_ipa` 2.3.0's IDF-6 build carries `zba1p0_zbb1p0_zbs1p0`, the B extension the P4 grew in
revision 3.0, while everything compiled from source in this tree (our code, `esp_video`) is
built without it because `sdkconfig.defaults` selects the <3.0 family. A prebuilt library
does not read `sdkconfig`. The same component ships an IDF-5.5 archive built for the older
ISA (`xesploop` only); it links against 6.0.2 with no undefined symbols, and on it the car
boots once and stays up with the sensor detected. `firmware/car/core/CMakeLists.txt` now
points the component's imported target at that archive whenever the <3.0 family is selected
— the vendor component itself is untouched, so a bump re-evaluates the question.

Two things this does not prove yet: that the IDF-5.5 archive's AE/AWB actually runs when the
pipeline streams (it only initialised here), and that nothing else in the tree hides a B
instruction behind a code path the boot did not take. `GET /snapshot` answers the first.

It also proved the rollback move made the same day for exactly this case would have worked
had the update gone over the air: the panic lands before `esp_ota_mark_app_valid_cancel_rollback`.
The cable flash had no rollback to offer, which is why the loop was visible at all.

### The chip is early silicon, and IDF 6.0 refuses it by default (2026-08-20)

The board carries an **ESP32-P4 revision v1.3**. ESP-IDF 6.0.2 defaults to `ESP32P4_REV_MIN_301`,
so the first flash stopped with:

```
'bootloader/bootloader.bin' requires chip revision in range [v3.1 - v3.99] (this chip is revision v1.3)
```

The two families are **mutually exclusive** in Kconfig — a build supports either `<3.0` or `>=3.0`,
never both — so the fix is two lines in `firmware/car/core/sdkconfig.defaults`:

```
CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y
CONFIG_ESP32P4_REV_MIN_100=y
```

which moves the supported range to `[v1.0 … v1.99]`. Everything else built and ran unchanged:
32 MB PSRAM at 200 MHz, 16 MB flash, both 4 MB OTA slots. If this project ever moves to a v3.x
board, both lines come out again.

### The native USB vanished, and the console followed the cable (2026-08-20)

`/dev/cu.usbmodem*` was present at the start of the session and never reappeared after the cable
was first moved — fifteen minutes of polling saw nothing, and no Espressif device was on the USB
bus at all, only the CH343P. The board has a **host/device jumper**; the P4's USB-OTG role is the
likely explanation, since in HOST the board is not a device for the computer to enumerate. That is
inference, not documentation: Waveshare's page describes the two Type-C ports only as "power
supply, program flashing, and debugging" and "power supply and program flashing", and says nothing
about the jumper.

Rather than keep hunting for the port, the fix `bringup.md` already prescribed was applied — move
the console, not the cable:

```
CONFIG_ESP_CONSOLE_UART_DEFAULT=y
CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=n
```

Logs and console then ride UART0 out through the CH343P bridge. This started as a bench-local
override and is now the committed default, once it became clear the native USB was not coming
back and that the choice costs nothing: ESP-IDF keeps USB-Serial-JTAG as the *secondary* console,
so log output reappears there by itself if that port ever returns. The reverse arrangement is not
available, which is what settles it — with USB-Serial-JTAG primary there is no UART secondary, and
a board whose native USB is silent would have no console at all.

Console *input* was broken by the same move at first, since `read_line()` read USB-Serial-JTAG
directly. That is now fixed properly rather than worked around: `console_init()` and
`console_read_byte()` are selected by `CONFIG_ESP_CONSOLE_*`, so the REPL follows the console
wherever ESP-IDF puts it, and a build with neither peripheral fails at compile time instead of
hanging on a silent read. Flashing, logs and typing all go through the one bridge port now.

One practical bonus: the CH343P does **not** re-enumerate when the chip resets, so its port name
stays stable across flashes — unlike the native USB, whose `usbmodemNNNN` number changes every time.

### The header's I2C is labelled by function, not by GPIO number (2026-08-21)

Waveshare publishes the 40-pin header only as an image, which is why no amount of reading the wiki
text finds it. Read the picture and the layout is the Raspberry Pi one, pin for pin:

| # | Left | # | Right |
|---|---|---|---|
| 1 | 3V3 | 2 | 5V |
| 3 | **SDA (GPIO7)** | 4 | 5V |
| 5 | **SCL (GPIO8)** | 6 | GND |
| 7 | GPIO23 | 8 | TXD (GPIO37) |
| 9 | GND | 10 | RXD (GPIO38) |
| 11 | GPIO21 | 12 | GPIO22 |
| 13 | GPIO20 | 14 | GND |
| 15 | GPIO6 | 16 | GPIO5 |
| 17 | 3V3 | 18 | GPIO4 |
| 19 | GPIO3 | 20 | GND |
| 21 | GPIO2 | 22 | GPIO1 |
| 23 | GPIO0 | 24 | GPIO36 |
| 25 | GND | 26 | GPIO32 |
| 27 | GPIO24 | 28 | GPIO25 |
| 29 | GPIO33 | 30 | GND |
| 31 | GPIO26 | 32 | **GPIO54 — C6 reset, leave alone** |
| 33 | GPIO48 | 34 | GND |
| 35 | GPIO53 | 36 | GPIO46 |
| 37 | GPIO47 | 38 | GPIO27 |
| 39 | GND | 40 | GPIO45 |

The trap is that pins 3 and 5 are silkscreened `SDA` and `SCL`, not `GPIO7` and `GPIO8`, so looking
for the numbers on the board finds nothing and the obvious conclusion — "those pins aren't brought
out" — is wrong.

Note what else is on that bus: the on-board **ES8311 audio codec at `0x18`**. That is a gift for
bring-up, because it makes a bus scan self-verifying. `0x18` present and the PCA9685s absent means
the pins and the bus are fine and the fault is in the boards' own wiring or power.

### The rear PCA9685's address is whatever its pads say (2026-08-21)

This file assumed `0x41`, from bridging **A0**. The board on the bench came back as **`0x60`** —
`0x40 | 0x20`, which is **A5** bridged. The pads sit in a row A5…A0 and bridging the wrong end of it
is easy.

Nothing was re-soldered. A PCA9685's address is data, `board.h` is the file that holds data about
this board, and `BOARD_PCA_ADDR_REAR` took the real value in one line. Worth remembering as a
general shape: when the hardware disagrees with the code about an arbitrary value, change the code.

A full scan of the working bus, for reference — note `0x70`, which is the PCA9685 **All Call**
address and is enabled by default, so it is one response from both boards rather than a third device:

```
SCAN: device answers at 0x18   (ES8311 codec, on-board)
SCAN: device answers at 0x40   (front axle)
SCAN: device answers at 0x60   (rear axle)
SCAN: device answers at 0x70   (PCA9685 All Call)
```

### The serial link survives full throttle (2026-08-21)

The pico car's standing warning is that its USB CDC port drops under motor load as VBUS sags, which
is why flashing-while-driving needs a separate 5 V supply. This board did not reproduce it: ten
seconds at duty 4095 on all four motors, and the CH343P bridge kept the console alive throughout —
the stop command went out over the same link that had just carried the drive command. Logic here is
powered over USB while the motors run from the battery, which is presumably why.

Worth not over-reading: one run, wheels off the ground. Under real load on the floor the answer may
differ.

### Both processors updated themselves over the air (2026-09-01)

Both boards were flashed by cable at `v1.0+747`, release `v1.0+749` was published, and nothing
touched either board again. They are now both running `v1.0+749`. The only route from one to the
other is the app.

That closes three debts at once, all of which had stood since the dongle was built:

- **The adapter's OTA had never run.** It has now.
- **The car's image crossed the adapter's TCP relay at 1.83 MB** — the image that carries the C6's
  firmware inside it, and roughly three times the largest payload the relay had handled before.
- **The unified firmware screen ran live**, having only ever been seen in the debug gallery.

The car's boot after it reads `radio firmware 3.0.6` with no line from `radio_flash` and no
rollback notice: the radio gate took its ordinary path on an image that carries a co-processor
firmware, compared, matched, and did nothing — and the new image was accepted by the bootloader.

**What this does NOT establish.** Nobody was watching, so there is no timing: whether the car's
upload finished inside the app's budget with room to spare or barely, whether anything retried,
and what the screens showed. The upload budget was widened from a flat 45 s to scale with the
image while this was in flight, so it is not even known which build performed it. Watching one
happen, with both consoles recording, is still owed.

### The radio gate ran for the first time, and said nothing (2026-09-01)

The car now carries the C6's image inside its own and offers it to a mismatched radio at boot
(`docs/superpowers/specs/2026-08-31-radio-in-one-image-design.md`). The first build carrying that
code went on by cable as `v1.0+747`, and the interesting part of the log is what is absent:

```
I (1480) app_init: App version:      v1.0+747
I (3730) status_api: radio firmware 3.0.6
```

No line from `radio_flash`, because the radio already matched. That is the designed ordinary
path — one string comparison, no RPC beyond the version read the car was already doing, nothing
written. For code that lives below `esp_ota_mark_app_valid_cancel_rollback()`, where a panic is a
permanent boot loop on a car with no cable, a first run that changes nothing is the only good
first run.

Still unproven, and only provable by breaking something on purpose: the flash itself, and the
three-attempt budget. Reflash the C6 by hand with an older image, reset the car, and watch it
correct itself; then make the flash impossible and confirm the car gives up and drives.

**A build-number trap worth knowing.** The dongle was reflashed from the same tree and reported
`v1.0+727` — the previous release's number. Nothing in `firmware/dongle` had changed, so CMake never
reconfigured, and the version string (derived from `git rev-list --count HEAD`) stayed at whatever
the last configure saw. The image was correct; only its label was stale. `idf.py reconfigure`
fixes it, and releases are immune because `tools/release.sh` runs `fullclean` first — but an
ordinary bench build can lie about its own version, which is exactly the kind of thing that wastes
an afternoon.

### The radio was updated over SDIO, with no wire at all (2026-08-20)

`firmware/car/modem/README.md` said the C6 is flashed through its UART header. It can also be updated
**from the host over the existing SDIO link**, and that is how it was done — no adapter, no
connector, nothing physical.

What made it work: the OTA calls are `RPC_ID__Req_OTAActivate = 266`, `OTABegin = 272`,
`OTAWrite = 273`, `OTAEnd = 274` — all from the original ESP-Hosted RPC set, which the shipped
image implements. The one call that *did* fail on the old image,
`RPC_ID__Req_GetCoprocessorFwVersion = 350`, is a late addition; its 5 s timeout was the whole
reason `/status` could not report a radio version, and it cost five seconds of every boot.

The delivery trick was to embed the 1.15 MB co-processor image in a throwaway P4 build
(`EMBED_FILES`) and drive `begin → write → end → activate` from `app_main`. Nothing had to cross
the network, which matters because the car is its own AP. The app grew to 1.91 MB in a 4 MB slot.

Two things worth knowing for next time:

- **`activate()` fails on the old image** — RPC 266 times out, exactly as the vendor changelog
  warns ("only for slave FW >= v2.6.0"). It does not matter: the old co-processor applies the
  image itself on `end()` and reboots immediately, which surfaces on the host as
  `Unrecoverable host sdio state` and `TRANSPORT_FAILURE: restarting host`. That looks alarming
  and is not — it is the radio restarting into its new firmware underneath a live link.
- **A failed write is safe.** OTA lands in the inactive slot, so an interrupted transfer leaves the
  running image untouched.

After the update: versions match, `SDIO SW_AGGR negotiated (e2h=15872B h2e=15872B)` — so the
ESP-IDF patch this project carries finally earns its keep, where the old image had forced a
fallback to `compatible streaming mode` — and boot reaches `Ready` in 3.7 s instead of 8.7 s.
