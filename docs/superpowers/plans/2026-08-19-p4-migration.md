# ESP32-P4 Migration (AJMiddleCar) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stand up `AJMiddleCar` — the same car, feature for feature, running on a Waveshare ESP32-P4-Module-DEV-KIT, in a repository where the firmware and the iOS pult are peers rather than host and guest.

**Architecture:** The new repo is a clone of AJPicoCar with full history, restructured into `app/` + `firmware/p4/` + `firmware/c6/`. The firmware is almost chip-independent already, so the port is a retarget: pin numbers move into `board.h`, `sdkconfig.defaults` switches to `esp32p4` with a 16 MB partition table, and WiFi arrives through `esp_wifi_remote` talking over SDIO to the on-board ESP32-C6 acting as a modem. The iOS app is forked with its own identity. Hardware is verified last, so every hardware assumption is quarantined in one header and one checklist.

**Tech Stack:** ESP-IDF 6.0.2 (RISC-V, esp32p4), `esp_wifi_remote`/`esp_hosted` over SDIO, cJSON, FreeRTOS; SwiftUI + XcodeGen; Python/aiohttp for the mock car.

**Spec:** `docs/superpowers/specs/2026-08-19-p4-migration-design.md`

## Global Constraints

- **ESP-IDF 6.0.2**, installed at `~/esp/esp-idf-v6.0.2`. `~/esp/esp-idf` (v5.4) is left untouched so AJPicoCar keeps building.
- **Target:** `esp32p4`.
- **Local directory:** `~/VSCode/esp32-p4-car`. **GitHub repo:** `ajohnson-smarthome/AJMiddleCar`, public, MIT.
- **softAP SSID:** `AJMiddleCar`, password `drive1234`.
- **Device identifier in `/status`:** `ajmiddlecar`.
- **iOS:** product `AJMiddleCar`, bundle id `com.adamjohnson.ajmiddlecar`.
- **Release artifact:** `ajmiddlecar.bin`.
- **Feature parity.** The only behaviour that differs from AJPicoCar is: the device identifier, the radio-version field in `/status`, and the network name. Nothing else is added, removed or redesigned.
- **Pin numbers in `board.h` are unverified placeholders** until bring-up. They must compile; they are not claimed to be correct.
- Commit after every task. Work directly on `main` in the new repo (matching AJPicoCar's workflow).

---

## File Structure

**Created:**

| File | Responsibility |
|---|---|
| `firmware/p4/main/board.h` | Every assumption about the physical board: I2C pins, bus speed, PWM frequency, expected radio firmware version. The only file bring-up edits. |
| `firmware/p4/main/identity.h` | Product identity: device id, SSID, password. Separate from `board.h` because it is about *which car*, not *which board*. |
| `firmware/p4/partitions.csv` | 16 MB layout: two 4 MB OTA slots plus a reserved data partition. |
| `firmware/p4/main/idf_component.yml` | Pinned `esp_wifi_remote` dependency. |
| `firmware/c6/` | The radio's build project: builds the `esp_hosted` slave for the C6. Knows nothing about the car. |
| `tools/flash-radio.sh` | Builds and flashes the slave through the board's C6 UART header. |
| `docs/protocol.md` | The wire contract between `app/` and `firmware/p4/` — the seam that makes the boundary an agreement rather than a folder convention. |
| `docs/bringup.md` | Bench checklist closing the six open assumptions. |

**Modified:** `firmware/p4/main/main.c` (pins and SSID move out), `status_api.c` (identity + radio), `wifi_ap.c` (uses `identity.h`), `sdkconfig.defaults`, `CMakeLists.txt`, `tools/release.sh`, `tools/mock_car/mock_car.py`, the whole `app/` tree's identity, `CLAUDE.md`, `README.md`.

**Moved wholesale (no content change):** every other `main/*.c`, `test/*`, the iOS sources, the inherited `docs/superpowers/`.

---

### Task 1: Create AJMiddleCar and restructure it

**Files:**
- Create: the whole repository at `~/VSCode/esp32-p4-car`
- Modify: nothing yet — this task only moves files

**Interfaces:**
- Produces: the directory layout every later task addresses (`app/`, `firmware/p4/`, `firmware/c6/`, `tools/`, `docs/`)

- [ ] **Step 1: Clone with full history**

```bash
git clone /Users/adamjohnson/VSCode/esp32-c6-car /Users/adamjohnson/VSCode/esp32-p4-car
cd /Users/adamjohnson/VSCode/esp32-p4-car
git remote remove origin
```

- [ ] **Step 2: Verify the history came across**

```bash
git log --oneline | wc -l    # expect ~460+
git log -1 --oneline         # expect the spec-review commit
```

- [ ] **Step 3: Restructure with `git mv` in one commit**

```bash
mkdir -p firmware/p4 firmware/c6 app
git mv CMakeLists.txt sdkconfig.defaults version.txt main test firmware/p4/
git mv ios/project.yml app/project.yml
git mv ios/ESP32Car app/AJMiddleCar
git mv ios/tests app/tests 2>/dev/null || true
git rm -r --cached ios 2>/dev/null || true; rm -rf ios
git mv tools/mock_car tools/mock_car    # already correct, no-op guard
```

Confirm nothing was left behind: `ls` should show only `app firmware tools docs CLAUDE.md README.md LICENSE .gitignore` plus dotfiles.

- [ ] **Step 4: Fix `.gitignore` for the new paths**

Replace the path-bearing lines (`build/`, `ios/build/`, `ios/ESP32Car.xcodeproj/`, `ios/DerivedData/`, `test/test_*`) with:

```
firmware/p4/build/
firmware/c6/build/
app/build/
app/AJMiddleCar.xcodeproj/
app/DerivedData/
firmware/p4/test/test_*
!firmware/p4/test/test_*.c
```

Keep the rest of the file as it is.

- [ ] **Step 5: Commit the restructure**

```bash
git add -A
git commit -m "chore: restructure into app/ + firmware/p4 + firmware/c6"
```

- [ ] **Step 6: Verify blame survived the move**

```bash
git log --follow --oneline firmware/p4/main/recovery.c | tail -3
```
Expected: the original `recovery.c` commits, not a single "restructure" commit.

- [ ] **Step 7: Create the GitHub repo and push**

```bash
gh repo create AJMiddleCar --public --source=. --remote=origin \
  --description "4WD RC car on ESP32-P4 with a native iOS pult"
git push -u origin main
```

---

### Task 2: Prove the pure modules survived the move

**Files:**
- Modify: `firmware/p4/test/Makefile` (only if its relative paths broke)
- Test: `firmware/p4/test/` — the existing host tests

**Interfaces:**
- Consumes: the layout from Task 1
- Produces: the first green gate; nothing later depends on its output

- [ ] **Step 1: Run the host tests in the new location**

```bash
cd firmware/p4/test && make run
```
Expected: all suites pass. `test/` and `main/` stayed siblings inside `firmware/p4/`, so relative includes should be unaffected.

- [ ] **Step 2: If anything failed, fix only the paths**

The failure mode to expect is a broken `../main/...` include, not a logic failure. Do not change test logic — these tests passed before the move and the move changed no code.

- [ ] **Step 3: Commit if anything changed**

```bash
git add firmware/p4/test/Makefile
git commit -m "test: fix host-test paths after the restructure"
```

---

### Task 3: Install ESP-IDF 6.0.2 alongside 5.4

**Files:**
- Create: `~/esp/esp-idf-v6.0.2` (outside the repo)

**Interfaces:**
- Produces: a shell environment where `idf.py --version` reports 6.0.2 and `esp32p4` is a valid target

- [ ] **Step 1: Clone the v6.0.2 tag**

```bash
git clone -b v6.0.2 --depth 1 --recursive https://github.com/espressif/esp-idf.git ~/esp/esp-idf-v6.0.2
```
If submodule fetches stall (a known failure on slow links), apply the tolerance settings first:
```bash
git config --global http.lowSpeedLimit 0
git config --global http.postBuffer 524288000
cd ~/esp/esp-idf-v6.0.2 && git submodule update --init --recursive
```

- [ ] **Step 2: Install the toolchain for esp32p4 only**

```bash
cd ~/esp/esp-idf-v6.0.2 && ./install.sh esp32p4
```
This creates its own Python virtualenv. The 5.4 install and its venv are untouched.

- [ ] **Step 3: Verify both IDFs still work independently**

```bash
bash -c 'source ~/esp/esp-idf-v6.0.2/export.sh >/dev/null 2>&1 && idf.py --version'
```
Expected: `v6.0.2`.

Then confirm 5.4 is intact — the old project must keep building:
```bash
bash -c 'export PATH=/tmp/py313bin:$PATH; source ~/esp/esp-idf/export.sh >/dev/null 2>&1 && idf.py --version'
```
Expected: `v5.4`.

- [ ] **Step 4: Record the environment in the repo**

Create `tools/env-p4.sh`:

```bash
#!/usr/bin/env bash
# Source this to build the P4 firmware:  source tools/env-p4.sh
source ~/esp/esp-idf-v6.0.2/export.sh
```

```bash
chmod +x tools/env-p4.sh
git add tools/env-p4.sh
git commit -m "build: pin the P4 firmware to ESP-IDF 6.0.2"
```

Note: if v6.0.2's installer needs no Python shadowing, the 5.4-era `/tmp/py313bin` workaround does not apply here — do not copy it in.

---

### Task 4: Retarget to the P4 — board, identity, partitions

**Files:**
- Create: `firmware/p4/main/board.h`, `firmware/p4/main/identity.h`, `firmware/p4/partitions.csv`
- Modify: `firmware/p4/main/main.c`, `firmware/p4/main/wifi_ap.c` call site, `firmware/p4/sdkconfig.defaults`

**Interfaces:**
- Consumes: the IDF 6.0.2 environment from Task 3
- Produces: `BOARD_I2C_SDA`, `BOARD_I2C_SCL`, `BOARD_I2C_HZ`, `BOARD_PWM_HZ`, `BOARD_RADIO_SLAVE_FW` from `board.h`; `CAR_DEVICE_ID`, `CAR_AP_SSID`, `CAR_AP_PASS` from `identity.h`

- [ ] **Step 1: Write `firmware/p4/main/board.h`**

```c
#ifndef BOARD_H
#define BOARD_H

// Everything this firmware assumes about the physical board lives here, and only here.
// Waveshare ESP32-P4-Module-DEV-KIT: ESP32-P4NRW32 + ESP32-C6 (radio, over SDIO),
// 32 MB PSRAM, 16 MB flash, 28 programmable GPIOs on the 2x20 header.
//
// UNVERIFIED until bring-up (see docs/bringup.md). These values compile; they are not
// claimed to be correct. The SDIO link to the C6 occupies a fixed GPIO group and the
// I2C pins must not collide with it — confirm against the board pinout before wiring.
#define BOARD_I2C_SDA        20
#define BOARD_I2C_SCL        21

#define BOARD_I2C_HZ         400000
#define BOARD_PWM_HZ         1000

// Expected esp_hosted slave version on the C6; filled in once the component is pinned
// in Task 5 and the real version is read in Task 6.
#define BOARD_RADIO_SLAVE_FW "unknown"

#endif // BOARD_H
```

- [ ] **Step 2: Write `firmware/p4/main/identity.h`**

```c
#ifndef IDENTITY_H
#define IDENTITY_H

// Which car this is — as opposed to board.h, which is about which board it runs on.
// The device id is how the app refuses to drive the wrong car: both cars serve the same
// API at the same address, so the SSID alone does not protect anyone.
#define CAR_DEVICE_ID  "ajmiddlecar"
#define CAR_AP_SSID    "AJMiddleCar"
#define CAR_AP_PASS    "drive1234"   // >= 8 chars for WPA2

#endif // IDENTITY_H
```

- [ ] **Step 3: Point `main.c` at the two new headers**

In `firmware/p4/main/main.c`, add `#include "board.h"` and `#include "identity.h"`, then delete these six defines:

```c
#define I2C_SDA_PIN  22
#define I2C_SCL_PIN  23
#define I2C_FREQ_HZ  400000
#define PWM_FREQ_HZ  1000
#define AP_SSID      "ESP32-Car"
#define AP_PASSWORD  "drive1234"
```

and update the three call sites:

```c
    ESP_ERROR_CHECK(pca9685_bus_init(BOARD_I2C_SDA, BOARD_I2C_SCL, BOARD_I2C_HZ));
    ESP_ERROR_CHECK(pca9685_init(BOARD_PWM_HZ));
    ...
    ESP_ERROR_CHECK(wifi_ap_start(CAR_AP_SSID, CAR_AP_PASS));
```

`WDT_TIMEOUT_MS` stays in `main.c` — it is a behaviour constant, not a board fact.

- [ ] **Step 4: Write `firmware/p4/partitions.csv`**

```
# Name,     Type, SubType,  Offset,   Size
nvs,        data, nvs,      0x9000,   0x6000
otadata,    data, ota,      0xf000,   0x2000
phy_init,   data, phy,      0x11000,  0x1000
ota_0,      app,  ota_0,    0x20000,  0x400000
ota_1,      app,  ota_1,    0x420000, 0x400000
storage,    data, fat,      0x820000, 0x7E0000
```

- [ ] **Step 5: Rewrite `firmware/p4/sdkconfig.defaults`**

```
CONFIG_IDF_TARGET="esp32p4"

# Console: main.c's `mix <t> <y>` REPL reads USB Serial JTAG directly.
# Which of the board's two Type-C ports carries it is confirmed at bring-up.
CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y
CONFIG_ESP_CONSOLE_UART_DEFAULT=n

CONFIG_HTTPD_WS_SUPPORT=y

# 16 MB flash, custom table: two 4 MB OTA slots plus a reserved data partition.
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y

# The module stacks 32 MB PSRAM. Feature parity needs none of it, but the setting is
# made deliberately rather than inherited: enabled, with nothing allocated there yet.
CONFIG_SPIRAM=y
```

- [ ] **Step 6: Build, and expect to fail at WiFi**

```bash
cd firmware/p4 && source ../../tools/env-p4.sh && idf.py set-target esp32p4 && idf.py build
```
Expected: every module of ours compiles; the build fails on `esp_wifi` (the P4 has no native WiFi, so the component is unavailable until Task 5 adds `esp_wifi_remote`).

**This failure is the gate.** It proves the IDF 6.0.2 upgrade did not break 5.4-era code — open assumption 5 from the spec. If instead our own `.c` files produce errors, fix those here; they belong to this task, not the next one.

- [ ] **Step 7: Commit**

```bash
git add firmware/p4/
git commit -m "feat(fw): retarget to ESP32-P4 — board.h, identity.h, 16MB partitions"
```

---

### Task 5: Wire the radio through `esp_wifi_remote`

**Files:**
- Create: `firmware/p4/main/idf_component.yml`
- Modify: `firmware/p4/main/main.c` (transport init), `firmware/p4/sdkconfig.defaults` (SDIO transport), `firmware/p4/main/CMakeLists.txt` if the component needs a REQUIRES entry

**Interfaces:**
- Consumes: the P4 target from Task 4
- Produces: a firmware image that builds and links — the main gate of the whole port

- [ ] **Step 1: Declare the dependency**

```bash
cd firmware/p4 && source ../../tools/env-p4.sh && idf.py add-dependency "espressif/esp_wifi_remote"
```
This writes `main/idf_component.yml`. Then pin it: open that file and replace any `*` or `^` version with the exact resolved version from `dependencies.lock`. A floating version on a transport component is how a working build silently becomes a broken one.

- [ ] **Step 2: Find the real transport-init symbol — do not guess it**

The exact function name and whether it must be called at all (some versions initialise via a system-init hook) is open assumption 4. Discover it from the fetched component:

```bash
grep -rn "esp_hosted_init\|esp_wifi_remote_init\|hosted_init" managed_components/ --include=*.h | head -20
ls managed_components/*/examples 2>/dev/null
```
Read the component's own example `app_main` and copy its ordering.

- [ ] **Step 3: Select SDIO transport**

Add to `firmware/p4/sdkconfig.defaults` the transport option found via:
```bash
idf.py menuconfig   # navigate to the ESP-Hosted / co-processor menu, note the symbol name
```
Record the exact `CONFIG_...=y` line rather than relying on a default.

- [ ] **Step 4: Call the transport init before WiFi**

In `app_main`, the hosted transport comes up **before** `wifi_ap_start()`. Nothing else in the init order changes:

```c
    ESP_ERROR_CHECK(<transport init from Step 2>);
    ESP_ERROR_CHECK(wifi_ap_start(CAR_AP_SSID, CAR_AP_PASS));
```

- [ ] **Step 5: Build to green**

```bash
cd firmware/p4 && source ../../tools/env-p4.sh && idf.py build
```
Expected: `Project build complete`, and a reported binary size well under the 4 MB slot.

If `esp_wifi_ap_get_sta_list` fails to link, that is open assumption 3 resolving the loud way: guard the call in `telemetry.c`'s `ap_client_rssi()` to return 0, which makes the app fall back to ping-based signal bars — a fallback it already implements. Record the outcome in `docs/bringup.md` either way.

- [ ] **Step 6: Commit**

```bash
git add firmware/p4/
git commit -m "feat(fw): WiFi over esp_wifi_remote — the C6 becomes a radio modem"
```

---

### Task 6: Device identity and radio version in `/status`

**Files:**
- Modify: `firmware/p4/main/status_api.c`, `firmware/p4/main/board.h`, `tools/mock_car/mock_car.py`

**Interfaces:**
- Consumes: `CAR_DEVICE_ID` (Task 4), a linking firmware (Task 5)
- Produces: `/status` fields `"device":"ajmiddlecar"` and `"radio":{...}`, consumed by the app in Tasks 9 and 10

- [ ] **Step 1: Use the identity constant in `/status`**

In `status_api.c`, `#include "identity.h"` and replace the hardcoded string:

```c
    int n = snprintf(buf, sizeof(buf), "{\"device\":\"" CAR_DEVICE_ID "\",\"fw\":\"%s\",%s}", fw, fields);
```

- [ ] **Step 2: Find the slave-version API — do not guess it**

```bash
grep -rn "version" firmware/p4/managed_components/*/include/*.h | grep -i "coprocessor\|slave\|fw" | head
```
Use the symbol you find. If no such API exists in the pinned version, report that and stop — `radio.fw` becomes `"unavailable"` and the mismatch check is dropped rather than faked.

- [ ] **Step 3: Report the radio in `/status`**

Extend the response with a nested object, reading the live version once at boot into a static:

```c
    char radio[96];
    snprintf(radio, sizeof(radio),
             "\"radio\":{\"fw\":\"%s\",\"expected\":\"%s\",\"ok\":%s}",
             s_radio_fw, BOARD_RADIO_SLAVE_FW,
             strcmp(s_radio_fw, BOARD_RADIO_SLAVE_FW) == 0 ? "true" : "false");
```

where `s_radio_fw` is a file-static filled once at boot, because querying the co-processor on every `/status` poll
would put SDIO traffic on a 1.5 s timer for a value that cannot change without a reboot:

```c
static char s_radio_fw[32] = "unavailable";
```

Populate it from `status_api_start()` using the symbol found in Step 2. Append the object inside the JSON and grow
`buf` — the current 224 bytes is already nearly full. Log a warning at boot on mismatch.

- [ ] **Step 4: Update `BOARD_RADIO_SLAVE_FW`**

Replace `"unknown"` with the version the pinned component actually ships, discovered in Step 2.

- [ ] **Step 5: Teach the mock the same two fields**

In `tools/mock_car/mock_car.py`, change the `/status` payload's `device` to `ajmiddlecar` and add a matching `radio` object with `ok: true`. The mock exists to be indistinguishable from the car at the protocol level; a mock that still says `esp32-car` would make Task 9 untestable.

- [ ] **Step 6: Verify against the mock**

```bash
cd tools/mock_car && .venv/bin/python -u mock_car.py &
sleep 1 && curl -s http://127.0.0.1:8080/status | python3 -m json.tool
```
Expected: `"device": "ajmiddlecar"` and a `radio` object.

- [ ] **Step 7: Build and commit**

```bash
cd firmware/p4 && source ../../tools/env-p4.sh && idf.py build
git add firmware/p4/ tools/mock_car/
git commit -m "feat(fw): distinct device identity + radio version in /status"
```

---

### Task 7: The radio's build project and flashing script

**Files:**
- Create: `firmware/c6/CMakeLists.txt`, `firmware/c6/sdkconfig.defaults`, `firmware/c6/main/` (whatever the slave project requires), `firmware/c6/README.md`, `tools/flash-radio.sh`

**Interfaces:**
- Consumes: the pinned `esp_hosted` version from Task 5
- Produces: a reproducible slave image; nothing in the P4 firmware depends on it at build time

- [ ] **Step 1: Determine how the slave is obtained**

This is open assumption 6, and it is structural. Check whether the slave is a project we can pin thinly:

```bash
source tools/env-p4.sh
idf.py create-project-from-example "espressif/esp_hosted:slave" 2>&1 | head
```
Also list what the component publishes:
```bash
grep -rn "slave" ~/.espressif/ --include=idf_component.yml 2>/dev/null | head
```

- [ ] **Step 2: Build the slave for the C6**

Whatever shape Step 1 revealed, land it under `firmware/c6/` and build:

```bash
cd firmware/c6 && source ../../tools/env-p4.sh && idf.py set-target esp32c6 && idf.py build
```
Note this needs the esp32c6 toolchain in the 6.0.2 install; add it with `~/esp/esp-idf-v6.0.2/install.sh esp32c6` if the target is rejected.

If the slave turns out to ship only as a copy-whole-example, say so in `firmware/c6/README.md` and record the exact version copied. Do not pretend it is a thin project if it is not.

- [ ] **Step 3: Write `tools/flash-radio.sh`**

```bash
#!/usr/bin/env bash
# Flash the ESP32-C6 radio co-processor. One-time bench procedure — see firmware/c6/README.md.
set -euo pipefail
cd "$(dirname "$0")/../firmware/c6"
source ../../tools/env-p4.sh
idf.py set-target esp32c6
idf.py build
echo "Connect a USB-serial adapter to the board's ESP32-C6 UART header, then:"
echo "  idf.py -p <port> flash"
```

- [ ] **Step 4: Write `firmware/c6/README.md`**

Cover: what this image is (a WiFi/BT modem for the P4, not car firmware), the pinned version, how to build it, how to flash it through the C6 UART header, and how to check the result — `/status`'s `radio.ok` after the P4 boots.

- [ ] **Step 5: Commit**

```bash
git add firmware/c6/ tools/flash-radio.sh
git commit -m "feat(radio): build project and bench procedure for the C6 slave image"
```

---

### Task 8: Fork the app's identity

**Files:**
- Modify: `app/project.yml`, `app/AJMiddleCar/Info.plist`, `app/AJMiddleCar/Theme.swift`, `app/AJMiddleCar/L.swift`, `app/AJMiddleCar/Resources/ru.lproj/Localizable.strings`

**Interfaces:**
- Produces: an app that installs alongside the pico app instead of replacing it

- [ ] **Step 1: Rename product and bundle id in `app/project.yml`**

Set the target name to `AJMiddleCar`, `PRODUCT_BUNDLE_IDENTIFIER` to `com.adamjohnson.ajmiddlecar`, and the sources path to `AJMiddleCar`. Every other setting — deployment target, orientation lock, development region `ru` — stays.

- [ ] **Step 2: Change the accent colour**

`Theme.swift` defines the accent twice — a brighter one for one palette, a deeper one for the other. Replace both
green values with blue, keeping the same bright/deep relationship so neither theme loses contrast:

```swift
        accent:    Color(red: 0.290, green: 0.612, blue: 0.949),   // was 0.290, 0.871, 0.502
```
```swift
        accent:    Color(red: 0.082, green: 0.361, blue: 0.639),   // was 0.082, 0.502, 0.239
```

Change only these two lines. Do not touch `warn` — it is amber and must stay distinguishable from the accent.

Note what this does not achieve: the app has **no custom icon** (no asset catalog, no `AppIcon`), so on the home
screen the two apps are told apart by their name only. Adding an icon is outside parity. The protection against
driving the wrong car is Task 9, not the colour.

- [ ] **Step 3: Change the app's display name**

Update the name string in `L.swift` / `Localizable.strings` to the middle car's name.

- [ ] **Step 4: Generate and build**

```bash
cd app && xcodegen generate
xcodebuild build -scheme AJMiddleCar -destination 'platform=iOS Simulator,name=iPhone 17' -derivedDataPath /tmp/ddata-middle 2>&1 | grep -iE "error:|BUILD SUCCEEDED|BUILD FAILED" | head
```
Expected: `BUILD SUCCEEDED`.

- [ ] **Step 5: Commit**

```bash
git add app/
git commit -m "feat(app): fork identity — AJMiddleCar product, bundle id, accent"
```

---

### Task 9: The app refuses a car that is not its own

**Files:**
- Modify: `app/AJMiddleCar/CarStatus.swift`, `app/AJMiddleCar/AppFlow.swift`, `app/AJMiddleCar/L.swift`
- Create: `app/AJMiddleCar/WrongCarView.swift`

**Interfaces:**
- Consumes: `/status` `device` field from Task 6
- Produces: `CarStatus.foreignDevice: String?` — non-nil when a reachable board reports someone else's identifier

- [ ] **Step 1: Add the expected identifier and detect a mismatch**

In `CarStatus.swift`, the bootstrap currently accepts any board answering `"esp32-car"`. Replace that with an explicit constant and a distinct mismatch state:

```swift
    static let expectedDevice = "ajmiddlecar"
    @Published var foreignDevice: String?     // non-nil: reachable, but the wrong car
```

In `bootstrap()`, when the JSON parses and `device` is present but differs from `expectedDevice`, set `foreignDevice` to the reported value and leave `online` false. A wrong car must not read as an offline car — the whole point is that the failure explains itself.

- [ ] **Step 2: Write `WrongCarView.swift`**

Follow the existing screen pattern exactly: `SplitScreen` draws its own header, and the app is landscape-locked, so
do **not** add a `.navigationTitle` or any system nav bar — that reintroduces the inset that shifts content.

```swift
import SwiftUI

/// Shown when a reachable board answers /status with someone else's device identifier.
/// A wrong car is not an offline car, and must not look like one.
struct WrongCarView: View {
    let found: String
    @Environment(\.colorScheme) private var scheme

    var body: some View {
        let p = Theme.palette(scheme)
        SplitScreen(palette: p, title: L.wrongCarTitle, onBack: nil) {
            FirmwareCarView(palette: p)
        } panel: {
            VStack(alignment: .leading, spacing: 12) {
                Text(L.wrongCarTitle).font(.title2.weight(.semibold)).foregroundStyle(p.text)
                Text(L.wrongCarSub(found, CarStatus.expectedDevice)).foregroundStyle(p.muted)
                Text(L.wrongCarHint(CarStatus.expectedSSID)).foregroundStyle(p.muted).font(.footnote)
            }
        }
    }
}
```

`CarStatus.expectedSSID` is a plain Swift constant holding `"AJMiddleCar"` — declare it next to `expectedDevice`.
Check the real `SplitScreen` signature before writing this and match it: the shape above states the intent, it is
not a signature to trust blindly.

- [ ] **Step 3: Route it in `AppFlow`**

Add a phase for the mismatch and show `WrongCarView` for it. It sits at the same point as the connect phase: reachable board, wrong identity.

- [ ] **Step 4: Strings**

Add the screen's strings to `L.swift` and `ru.lproj/Localizable.strings`. No Cyrillic literals in views — the codebase's rule.

- [ ] **Step 5: Test against a mock pretending to be the other car**

```bash
cd tools/mock_car
sed -i '' 's/"ajmiddlecar"/"esp32-car"/' mock_car.py
.venv/bin/python -u mock_car.py &
```
Launch the app in the simulator: it must show `WrongCarView`, not the drive screen and not "offline". Then revert the mock:
```bash
sed -i '' 's/"esp32-car"/"ajmiddlecar"/' mock_car.py
```
and confirm the app reaches the drive screen again.

- [ ] **Step 6: Commit**

```bash
git add app/
git commit -m "feat(app): refuse a foreign car instead of driving it"
```

---

### Task 10: Release plumbing

**Files:**
- Modify: `tools/release.sh`, `app/AJMiddleCar/UpdateClient.swift`

**Interfaces:**
- Consumes: nothing from earlier tasks
- Produces: releases the app can find under the new repo and artifact name

- [ ] **Step 1: Point `release.sh` at the new layout and artifact**

`BIN` becomes `firmware/p4/build/ajmiddlecar.bin`, and the build steps `cd firmware/p4` and source `tools/env-p4.sh` instead of the 5.4 environment. The `main`-branch and clean-tree guards stay as they are.

- [ ] **Step 2: Set the project name so the artifact matches**

In `firmware/p4/CMakeLists.txt`, `project(ajmiddlecar)`.

- [ ] **Step 3: Replace the fragile asset heuristic in `UpdateClient.swift`**

The current line takes the first asset whose name ends in `.bin`:

```swift
let bin = assets.first { ($0["name"] as? String)?.hasSuffix(".bin") == true }
```

Match the exact name instead:

```swift
let bin = assets.first { ($0["name"] as? String) == "ajmiddlecar.bin" }
```

Also update the GitHub repository path in the same file to `ajohnson-smarthome/AJMiddleCar`.

- [ ] **Step 4: Show the radio version on the Firmware screen**

The spec requires the app to surface `/status`'s `radio` field, and the Firmware screen is where firmware versions
already live. Without this step Task 6's work is invisible — nothing else consumes the field.

Decode `radio.fw` and `radio.ok` where `/status` is parsed for this screen, then add one line to `FirmwareView.swift`
under the existing current-version line (`sub(L.fwCurrent(current))` and its siblings): the radio version, plus —
when `ok` is false — a warning that the co-processor firmware does not match what this build expects, naming
`firmware/c6/README.md` as the fix. Add the strings to `L.swift` and `ru.lproj/Localizable.strings`; no Cyrillic
literals in views.

- [ ] **Step 5: Dry-run the release script**

```bash
tools/release.sh --dry-run
```
Expected: it prints the version, tag, title and `firmware/p4/build/ajmiddlecar.bin` as the asset, and exits without creating anything.

- [ ] **Step 6: Build the app and commit**

```bash
cd app && xcodegen generate && xcodebuild build -scheme AJMiddleCar -destination 'platform=iOS Simulator,name=iPhone 17' -derivedDataPath /tmp/ddata-middle 2>&1 | grep -iE "error:|BUILD SUCCEEDED|BUILD FAILED" | head
git add tools/release.sh firmware/p4/CMakeLists.txt app/
git commit -m "build: releases under the AJMiddleCar name and exact asset match"
```

---

### Task 11: Close the identity check on the pico side

**Files:**
- Modify: `/Users/adamjohnson/VSCode/esp32-c6-car/ios/ESP32Car/CarStatus.swift`

**This task is in the OTHER repository** — AJPicoCar. Without it the protection works in one direction only: the middle app refuses the pico car, but the pico app still happily drives the middle car.

**Interfaces:**
- Consumes: nothing
- Produces: symmetry — each app accepts only its own car

- [ ] **Step 1: Make the pico app's expectation explicit**

In `esp32-c6-car/ios/ESP32Car/CarStatus.swift`, the check `(j["device"] as? String) == "esp32-car"` already rejects a board reporting `ajmiddlecar` — it simply falls through as "not online", which reads as a broken car rather than the wrong one. Introduce the same `expectedDevice` constant and `foreignDevice` state as Task 9 so the failure is legible.

- [ ] **Step 2: Decide how far to carry it**

The minimum is the mismatch state plus a line in the existing status area naming the found car. A full `WrongCarView` port is optional here — the pico app is feature-frozen relative to this migration. Do the minimum and say so in the commit message.

- [ ] **Step 3: Build and commit in the pico repo**

```bash
cd /Users/adamjohnson/VSCode/esp32-c6-car/ios && xcodegen generate
xcodebuild build -scheme ESP32Car -destination 'platform=iOS Simulator,name=iPhone 17' -derivedDataPath /tmp/ddata 2>&1 | grep -iE "error:|BUILD SUCCEEDED|BUILD FAILED" | head
cd /Users/adamjohnson/VSCode/esp32-c6-car
git add ios/ESP32Car/CarStatus.swift
git commit -m "fix(ios): reject a car that reports a different device identifier"
```

---

### Task 12: Documentation

**Files:**
- Create: `docs/protocol.md`, `docs/bringup.md`
- Modify: `CLAUDE.md` (rewritten), `README.md`

**Interfaces:**
- Consumes: everything learned in Tasks 4–7, particularly the discovered symbol names and versions

- [ ] **Step 1: Write `docs/protocol.md`**

The seam between `app/` and `firmware/p4/`, written so either side could be reimplemented from it alone: the identity handshake (`GET /status`, the `device` field, what a mismatch means), the `/ws` control frame `{"t","y"}` with its 10 Hz streaming requirement and the 300 ms watchdog that requirement feeds, the 5 Hz telemetry frame and every field in it, each REST endpoint with its JSON body and accepted ranges, and `POST /ota`. Take the values from the code, not from memory.

- [ ] **Step 2: Write `docs/bringup.md`**

A checklist, one item per open assumption in the spec, each with the command that settles it and a blank for the answer: I2C pins against the real pinout; whether the C6 arrived pre-flashed; whether `esp_wifi_ap_get_sta_list` linked or was stubbed; the transport-init symbol used; whether 5.4-era code needed changes on 6.0.2; and what shape `firmware/c6/` ended up taking. Then the physical sequence: flash, join `AJMiddleCar`, `curl /status`, drive one wheel, drop WiFi mid-drive and confirm the watchdog stops the car.

- [ ] **Step 3: Rewrite `CLAUDE.md`**

Not a copy. The inherited file describes the XIAO's pin mapping, the flat layout and the python 3.13 workaround — all wrong here. Rewrite for this repo: the P4 board and its C6 radio, the `app/` + `firmware/p4` + `firmware/c6` layout, `source tools/env-p4.sh` as the build entry point, the identity rule, and the gotchas discovered during this migration.

- [ ] **Step 4: Rewrite `README.md`**

AJPico-family style, describing the middle car. Link the sibling repo.

- [ ] **Step 5: Commit**

```bash
git add docs/ CLAUDE.md README.md
git commit -m "docs: protocol contract, bring-up checklist, rewritten CLAUDE.md and README"
```

---

## Definition of done

- `cd firmware/p4/test && make run` — all host tests pass
- `cd firmware/p4 && idf.py build` — succeeds on IDF 6.0.2 for `esp32p4`, image well under 4 MB
- `cd firmware/c6 && idf.py build` — produces the slave image, or `firmware/c6/README.md` records why it takes a different shape
- `xcodebuild -scheme AJMiddleCar` — succeeds; the app installs alongside the pico app
- The app drives the mock, and shows `WrongCarView` when the mock claims to be `esp32-car`
- `tools/release.sh --dry-run` — names `ajmiddlecar.bin`
- `docs/bringup.md` exists with every assumption listed and unanswered — it is filled in at the bench, which is out of this plan's scope by the spec's own choice
