# Bring-up checklist

This project was built with the board on order rather than on the bench — a deliberate choice,
which is why every assumption about the hardware was quarantined instead of scattered. Three of
them are settled by building; the rest wait here.

Fill the answers in as you go. If an answer differs from what the code assumes, the fix is named
next to it.

## Open assumptions

Four of the six were settled at the desk, exactly as the spec predicted — by building, with no
board present. Two remain, and both are cheap to fix.

| # | Assumption | Status |
|---|---|---|
| 3 | `esp_wifi_remote` proxies `esp_wifi_ap_get_sta_list` | **Resolved — it links.** RSSI telemetry survives the port |
| 4 | An explicit hosted transport-init call in `app_main` | **Resolved — none needed.** `ESP_HOSTED_AUTO_CALL_INIT_BEFORE_APP_MAIN` brings the transport up before `app_main` |
| 5 | 5.4-era code builds unchanged on IDF 6.0.2 | **Resolved — one break.** cJSON left ESP-IDF for the component manager; declared in `idf_component.yml`. Nothing else needed changing |
| 6 | The `esp_hosted` slave builds as a project we own | **Resolved — it is theirs, and it builds.** A standard IDF project inside the pinned component; `firmware/c6/` holds only the procedure. Needs a vendor patch to ESP-IDF (`eh.py patch-idf`), applied automatically |
| 1 | The I2C pins in `board.h` are free and clear of the SDIO link | **Open — needs the board** |
| 2 | The C6 arrives already flashed with the slave image | **Open — needs the board** |

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
2. Run `firmware/c6/flash-radio.sh`, then set `BOARD_RADIO_SLAVE_FW` to the version flashed.
3. Already resolved; no action.

## Bench sequence

- [ ] **Work out the two Type-C ports.** They are not interchangeable: the board carries a
      **CH343P USB-UART bridge** and an **FSUSB42UMX mux** that switches the data path between the
      P4's native USB and that bridge. Our firmware puts the console on **USB-Serial-JTAG**
      (native USB) because `read_line()` in `main.c` reads that peripheral directly — so if
      flashing lands on the bridge port, the console is on the other one and you need both
      cables. Plug in, run `ls /dev/cu.*`, and record which port does what:

      | Port | Enumerates as | Flash? | Console? |
      |---|---|---|---|
      | Type-C #1 | | | |
      | Type-C #2 | | | |

      If one cable can do both, nothing needs changing. If not, the fix is to make console input
      independent of the peripheral rather than to move the console — `read_line()` is the only
      thing tying it to USB-Serial-JTAG.
- [ ] **Flash.** `cd firmware/p4 && source ../../tools/env-p4.sh && idf.py -p /dev/cu.usbmodem* flash monitor`
- [ ] **Radio.** `/status` → `radio.ok` is true. If false, assumption 2 above.
- [ ] **SDIO pull-ups.** Espressif requires external 51 kΩ pull-ups on `CMD` and `D0`–`D3`. On an
      integrated board they should be on the PCB — confirm, because missing pull-ups on `D2`/`D3`
      also let the slave drop into SPI mode at startup.
- [ ] **Network.** SSID `AJMiddleCar` appears; the phone joins it and gets an address on `192.168.4.x`.
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
