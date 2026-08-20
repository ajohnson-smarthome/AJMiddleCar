# Bring-up checklist

This project was built with the board on order rather than on the bench — a deliberate choice,
which is why every assumption about the hardware was quarantined instead of scattered. Three of
them are settled by building; the rest wait here.

Fill the answers in as you go. If an answer differs from what the code assumes, the fix is named
next to it.

## Open assumptions

Four of the six were settled at the desk, exactly as the spec predicted — by building, with no
board present. The board arrived on 2026-08-20; assumption 2 was answered that evening, and only
assumption 1 is still open, because the PCA9685 boards are not wired yet.

| # | Assumption | Status |
|---|---|---|
| 3 | `esp_wifi_remote` proxies `esp_wifi_ap_get_sta_list` | **Resolved — it links.** RSSI telemetry survives the port |
| 4 | An explicit hosted transport-init call in `app_main` | **Resolved — none needed.** `ESP_HOSTED_AUTO_CALL_INIT_BEFORE_APP_MAIN` brings the transport up before `app_main` |
| 5 | 5.4-era code builds unchanged on IDF 6.0.2 | **Resolved — one break.** cJSON left ESP-IDF for the component manager; declared in `idf_component.yml`. Nothing else needed changing |
| 6 | The `esp_hosted` slave builds as a project we own | **Resolved — it is theirs, and it builds.** A standard IDF project inside the pinned component; `firmware/c6/` holds only the procedure. Needs a vendor patch to ESP-IDF (`eh.py patch-idf`), applied automatically |
| 2 | The C6 arrives already flashed with the slave image | **Resolved — flashed, but not with our version.** It shipped an image reporting `0.0.0`, four major versions behind the pinned 3.0.6. WiFi worked anyway; see the bench notes for how it was updated |
| 1 | The I2C pins in `board.h` are free and clear of the SDIO link | **Still open — the PCA9685 boards are not wired yet.** With nothing on the bus, `0x40` is silent and boot aborts in `pca9685_init`, which is the designed behaviour and tells us nothing about the pins |

**What is now known about the pins.** SDIO reserves **GPIO 14–19** on the P4 (D0–D3, CLK, CMD)
plus **GPIO 54** for co-processor reset, pinned in `firmware/p4/sdkconfig.defaults`. `board.h`
currently puts I2C on **GPIO 20 and 21**, which does not collide — but those numbers were chosen
to compile, not from the board's silkscreen.

Two things to check against the actual pinout:

1. Whether GPIO 20/21 are brought out on the 2×20 header and free. If not, change
   `BOARD_I2C_SDA` / `BOARD_I2C_SCL` — one line each, and nothing else in the firmware knows.
2. Whether Waveshare wires the C6 to the same GPIOs the component defaults to. If not, the
   `CONFIG_ESP_HOSTED_HOST_SDIO_PIN_*` lines in `sdkconfig.defaults` must change too — the
   firmware would otherwise talk to the radio over the wrong wires and simply find nothing.

**Fixes if an answer is no:**

1. See above.
2. Answered, and the fix turned out not to need the wire the README assumed — the co-processor
   was updated over SDIO from the host. `firmware/c6/README.md` now carries both procedures.
3. Already resolved; no action.

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
- [x] **Flash.** `cd firmware/p4 && source ../../tools/env-p4.sh && idf.py -p /dev/cu.usbmodem* flash monitor`
      — works on either port. Needed one config change first; see the bench notes on chip revision.
- [x] **Radio.** `esp-hosted fw versions: host=3.0.6 coprocessor=3.0.6 (match)`, and `status_api`
      logs `radio firmware 3.0.6`, which it only does when `radio.ok` is true.
- [x] **SDIO pull-ups.** Present. Not inspected visually — inferred from behaviour, which is
      stronger: the link comes up as `SDIO 4-bit 40000 kHz` and reports `Card init success`,
      which is exactly what missing `D2`/`D3` pull-ups would prevent by dropping the slave to SPI.
- [ ] **Network.** SSID `AJMiddleCar` appears; the phone joins it and gets an address on `192.168.4.x`.
      Half-answered: the AP starts and the DHCP server binds `192.168.4.1`, but no client has
      associated yet.
- [ ] **Identity.** `curl http://192.168.4.1/status` returns `"device":"ajmiddlecar"`.
- [ ] **I2C.** **Both** PCA9685 boards answer — `0x40` (front axle) and `0x41` (rear). If only
      one does, check the rear board's A0 jumper before suspecting anything else; boot fails
      loudly either way, naming which board did not answer. If neither answers, assumption 1.
- [ ] **One wheel.** Console `mix 0.5 0` — at least one motor turns. If nothing turns, this is
      almost always command delivery rather than firmware: opening the serial port resets the
      board, so a command sent in the first second or two is swallowed during boot.
- [ ] **Calibration.** Run the app's wizard end to end; `/status` reports `calibrated: true`,
      and it survives a power cycle.
- [ ] **Driving.** Both schemes, forward, reverse, tank turn.
- [ ] **Watchdog.** Drive, then drop WiFi mid-drive. The car must retrace and stop, and
      `wdt_trips` must increment. This has never been tested on the pico car either — it is the
      oldest untested behaviour in the family.
- [ ] **OTA.** Cut a release, let the app's launch gate force-update the board, confirm it boots
      and `fw` reports the new build.

## Notes from the bench

_Record anything surprising here — it is the raw material for the next spec._

### The chip is early silicon, and IDF 6.0 refuses it by default (2026-08-20)

The board carries an **ESP32-P4 revision v1.3**. ESP-IDF 6.0.2 defaults to `ESP32P4_REV_MIN_301`,
so the first flash stopped with:

```
'bootloader/bootloader.bin' requires chip revision in range [v3.1 - v3.99] (this chip is revision v1.3)
```

The two families are **mutually exclusive** in Kconfig — a build supports either `<3.0` or `>=3.0`,
never both — so the fix is two lines in `firmware/p4/sdkconfig.defaults`:

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

Logs and console then ride UART0 out through the CH343P bridge. This was kept out of the committed
defaults, as a bench-local override, because it is a workaround for one board's state and not a
property of the design. Note `read_line()` still reads USB-Serial-JTAG directly, so console *input*
does not work in this configuration — it was not needed, because the diagnostics were made to run
unattended at boot instead. Making input peripheral-independent is still the real fix if console
input is ever needed on the bridge.

One practical bonus: the CH343P does **not** re-enumerate when the chip resets, so its port name
stays stable across flashes — unlike the native USB, whose `usbmodemNNNN` number changes every time.

### The radio was updated over SDIO, with no wire at all (2026-08-20)

`firmware/c6/README.md` said the C6 is flashed through its UART header. It can also be updated
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
