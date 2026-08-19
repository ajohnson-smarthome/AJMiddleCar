# Bring-up checklist

This project was built with the board on order rather than on the bench — a deliberate choice,
which is why every assumption about the hardware was quarantined instead of scattered. Three of
them are settled by building; the rest wait here.

Fill the answers in as you go. If an answer differs from what the code assumes, the fix is named
next to it.

## Open assumptions

| # | Assumption | How it is settled | Answer |
|---|---|---|---|
| 1 | The I2C pins in `board.h` are free, and do not collide with the SDIO link to the C6 | Board pinout + `i2cdetect`-style scan for the PCA9685 at `0x40` | |
| 2 | The ESP32-C6 arrives with the `esp_hosted` slave image already flashed | Boot and read `/status` → `radio.fw`. If absent or wrong, `tools/flash-radio.sh` | |
| 3 | `esp_wifi_remote` proxies `esp_wifi_ap_get_sta_list`, and an unsupported call fails loudly | Build (link error) — otherwise it is a runtime stub and `rssi` reads 0 | |
| 4 | The hosted transport-init call and its position in `app_main` | Build | |
| 5 | 5.4-era code builds unchanged on IDF 6.0.2 | Build | |
| 6 | The `esp_hosted` slave builds as a standalone project under `firmware/c6/` | Build | |

**Fixes if an answer is no:**

1. Change `BOARD_I2C_SDA` / `BOARD_I2C_SCL` in `firmware/p4/main/board.h`. One line each; nothing
   else in the firmware knows the pin numbers.
2. Run `tools/flash-radio.sh`, then set `BOARD_RADIO_SLAVE_FW` to the version actually flashed.
3. Guard `ap_client_rssi()` in `telemetry.c` to return 0. The app already falls back to a
   latency-based signal indicator, so nothing breaks — the bars simply lose precision.

## Bench sequence

- [ ] **Flash.** `cd firmware/p4 && source ../../tools/env-p4.sh && idf.py -p /dev/cu.usbmodem* flash monitor`
      The board has two Type-C ports; note which one carries the USB-Serial-JTAG console and
      record it here, because `sdkconfig.defaults` assumes it exists.
- [ ] **Radio.** `/status` → `radio.ok` is true. If false, assumption 2 above.
- [ ] **Network.** SSID `AJMiddleCar` appears; the phone joins it and gets an address on `192.168.4.x`.
- [ ] **Identity.** `curl http://192.168.4.1/status` returns `"device":"ajmiddlecar"`.
- [ ] **I2C.** The PCA9685 answers at `0x40`. If not, assumption 1.
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
