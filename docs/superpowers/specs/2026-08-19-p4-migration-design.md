# ESP32-C6 → ESP32-P4 migration — AJMiddleCar

**Date:** 2026-08-19
**Scope:** a new repository (`AJMiddleCar`) holding a P4 port of this firmware plus its own iOS app.
This repo (`AJPicoCar`, the C6 car) is unaffected and keeps running.
**Board:** Waveshare ESP32-P4-Module-DEV-KIT (ESP32-P4NRW32 + ESP32-C6, 32 MB PSRAM, 16 MB flash).

## Goal

Run the existing car — same drive electronics, same protocol, same features — on the P4 board, and land it
in a repository whose layout draws a hard line between the firmware and the phone pult. Feature parity is
the finish line: the middle car drives exactly like the pico car.

Parity is about behaviour, not byte-identical APIs. This spec knowingly adds three things the pico car does not
have — a distinct device identifier, a radio-version field, and its own network name — because two cars sharing a
bench must be distinguishable. Nothing else is added.

The P4's real payload (camera/FPV, on-board display, lidar) is explicitly **not** part of this migration.
This spec exists so those can be built later on a board that already drives.

## The hardware fact that shapes everything

**ESP32-P4 has no radio.** On this board an **ESP32-C6 is wired to the P4 over SDIO** and acts as a WiFi/BT
modem. The C6 stops being the brain and becomes a peripheral.

Consequences:

- Application code keeps calling the ordinary `esp_wifi` API; the `esp_wifi_remote` component marshals those
  calls over SDIO. `wifi_ap.c` does not change.
- The car carries **two firmware images**: our application on the P4, and Espressif's `esp_hosted` slave image
  on the C6. The slave is a vendor artifact — we pin its version, we do not author it.
- 16 MB flash (vs 4 MB) removes the OTA-slot pressure the C6 build lived under (0.93 MB app, 9% free).

## Decisions

| Decision | Choice | Rationale |
|---|---|---|
| Scope | Clean port, feature parity | Prove the hardware drives before building on it |
| Old project | AJPicoCar stays alive | Two physical cars, two repos, code diverges over time |
| iOS app | Own app in the new repo | The apps will genuinely diverge (FPV/display are P4-only); a shared SPM package is premature |
| C6 slave image | Flashed by wire once, version pinned | Air-updating the radio needs WiFi down mid-transfer; disproportionate for a rare event |
| Layout | `app/` + `firmware/p4/` + `firmware/c6/` | No component owns the repo root |
| Order of work | Full port first, hardware verified at the end | User's call; mitigated by quarantining hardware assumptions in `board.h` and `docs/bringup.md` |
| ESP-IDF | 6.0.2 (latest stable, released 2026-03) | Best P4 and `esp_hosted` maturity; installed alongside 5.4, old project untouched |
| Code provenance | Clone AJPicoCar with full history | `git blame` explains the non-obvious constants (e.g. recovery's 400 ms tail cap) that cost debugging sessions |

## Creating the repository

`AJMiddleCar` is created by cloning this repo with its full history, then restructuring into the layout below in a
single `git mv` commit (rename detection keeps `git blame` intact across the move). It is public, MIT-licensed, and
keeps the AJPico-family README style, matching AJPicoCar.

This spec is committed to **AJPicoCar** deliberately: since the new repo starts as a clone, the document travels with
it and lands there as the fork-point record.

The clone also carries ~30 inherited specs and plans describing the C6 car. They stay, moved wholesale under
`docs/superpowers/` in the restructure commit: they are the design record of the code being ported, and the new
repo's own specs start alongside them from this one. Nothing is pruned — deleting the reasoning behind code you
have just inherited is how constants become mysterious.

## Repository layout

```
AJMiddleCar/
├── app/                  iOS pult (XcodeGen)
│   ├── project.yml
│   ├── AJMiddleCar/      sources
│   └── tests/
├── firmware/
│   ├── p4/               the car's firmware — all logic
│   │   ├── CMakeLists.txt
│   │   ├── sdkconfig.defaults
│   │   ├── partitions.csv
│   │   ├── version.txt            semver; must sit beside CMakeLists.txt
│   │   ├── main/
│   │   │   ├── board.h            the only file that knows the board
│   │   │   └── idf_component.yml  pinned esp_wifi_remote / esp_hosted
│   │   └── test/         host tests for the pure modules
│   └── c6/               radio: builds the esp_hosted slave image
│       ├── CMakeLists.txt
│       ├── sdkconfig.defaults     SDIO transport
│       └── README.md              wired-flash procedure via the C6 UART header
├── tools/                mock_car, release.sh, flash-radio.sh
├── docs/                 specs, plans, protocol.md, bringup.md
├── CLAUDE.md
└── README.md
```

Boundaries: `app/` and `firmware/p4/` do not reference each other. Their only seam is `docs/protocol.md`
(the wire contract) and `tools/mock_car` (an executable stand-in for the car). `firmware/c6/` knows nothing
about the car at all — not the motors, not the protocol.

The ESP-IDF project moves into `firmware/p4/`, so `idf.py` runs from there. This is the cost of the boundary:
build commands and script paths gain one level.

`version.txt` moves with `CMakeLists.txt`, which reads it as `${CMAKE_CURRENT_LIST_DIR}/version.txt`. Left behind at
the repo root it would not fail loudly — CMake would simply produce an empty semver and `PROJECT_VER` would become
garbage that only shows up as a wrong version string on the car.

## Port inventory

The firmware turned out to be almost entirely chip-independent. The total C6-specific surface is:

| Item | Size |
|---|---|
| I2C pin numbers | 2 lines in `main.c` (`GPIO22/23`) |
| `sdkconfig.defaults` | target, flash size, partition table |
| `esp_wifi` calls | 5 functions: `init`, `set_mode`, `set_config`, `start`, `ap_get_sta_list` |

**Ports untouched** — 2179 lines of C across 26 modules:

- Pure modules (`mixer`, `motors`, `control_proto`, `wheel`, `dims`, `cfg_json`, the telemetry formatter, the
  pure parts of `watchdog`/`recovery`/`calibration`) have no ESP-IDF dependency at all and are already host-tested.
- Car logic (`car`, `ramp`, `http_server`, `ws_control`, `status_api`, `ota_api`, all seven `*_api` modules) is
  written against portable APIs: `esp_http_server`, `esp_timer`, FreeRTOS, `nvs`, `esp_ota`.
- `wifi_ap.c` — unchanged, byte for byte.
- `pca9685.c` — already on the `i2c_master` API, target-agnostic; only the pin numbers move out.

**Changes:**

1. I2C pins move from `main.c` into a new `firmware/p4/main/board.h`
2. `sdkconfig.defaults`: target `esp32p4`, 16 MB flash, custom `partitions.csv`, SDIO transport for hosted, plus two
   board settings the C6 build never needed:
   - **Console** — `main.c`'s `mix <t> <y>` REPL reads USB Serial JTAG directly. The P4 has that peripheral, but the
     board exposes two Type-C ports and the console default may differ from the C6's; the routing is set explicitly
     and which physical port carries it is confirmed at bring-up.
   - **PSRAM** — the module stacks 32 MB. Parity needs none of it, but leaving the setting to whatever the target
     defaults to is not a decision. It is configured deliberately, and the choice is recorded here rather than
     inherited silently.
3. New `idf_component.yml` pinning `esp_wifi_remote` / `esp_hosted`
4. IDF 5.4 → 6.0.2: a major upgrade. Expect deprecated-API fixes (`esp_vfs_dev.h` was already deprecated in 5.4)
   and Picolibc-related `printf` formatting differences.

**New — and each of these is work, not a line in a tree diagram:**

- `firmware/c6/` — the radio's build project
- `board.h` — the board-assumption quarantine
- The `device` identifier and the radio-version field in `/status` (see *Telling the two cars apart*)
- `docs/bringup.md` — the bench checklist that closes the open assumptions
- `docs/protocol.md` — **written from scratch.** It is named as the seam between `app/` and `firmware/p4/`, but no
  such document exists today: the wire contract currently lives scattered through `CLAUDE.md`. Without it the
  boundary is a folder convention rather than an agreement. It documents the `/ws` control frame and telemetry, all
  REST endpoints with their JSON bodies and ranges, `/ota`, and the device identity handshake.
- `CLAUDE.md` — **rewritten, not copied.** The inherited one is 200+ lines about the C6: XIAO pin mapping, the old
  flat layout, the python 3.13 workaround. Carried over unedited it would mislead on roughly every second
  paragraph.

## Radio integration

`app_main` gains exactly one step: bring up the hosted transport **before** `wifi_ap_start()`. Nothing else in
the init order changes.

`board.h` quarantines every hardware assumption:

```c
// firmware/p4/main/board.h — the only file that knows what we are standing on
#define BOARD_I2C_SDA        /* from bring-up */
#define BOARD_I2C_SCL        /* from bring-up */
#define BOARD_I2C_HZ         400000
#define BOARD_PWM_HZ         1000
#define BOARD_RADIO_SLAVE_FW "2.x.y"   // expected esp_hosted slave version
```

This is the mitigation for verifying hardware last: when the board reaches the bench and I2C turns out to need
different pins, the fix is one line in one file. The pins are not free — SDIO to the C6 occupies a fixed GPIO
group and I2C must not collide with it. The 2×20 header exposes 28 programmable GPIOs, so there is room; the
exact numbers come from the board pinout.

**Radio diagnostics.** Since the slave is wire-flashed and its version pinned, a mismatch must be visible by
inspection rather than by symptom. At boot the firmware queries the slave version and reports it in `/status`:

```json
"radio": {"fw": "2.11.7", "expected": "2.11.7", "ok": true}
```

A mismatch logs a warning and sets `ok:false`. This is the only mention of the C6 in the car's code, and it is
diagnostics, not structure. The app surfaces it on the existing Firmware screen.

`firmware/c6/` builds the slave for target `esp32c6` against the same pinned `esp_hosted` version, and documents
the wired-flash procedure through the board's C6 UART header. The built `.bin` is a build artifact and is not
committed.

## Flash layout, versioning, releases

Partition tables are expensive to change later — a new table means a wired reflash — so the layout is sized for
where the P4 is going, not for what it does today:

```
# firmware/p4/partitions.csv
nvs,      data, nvs,      0x9000,   0x6000     # 24K, standard offset — settings
otadata,  data, ota,      0xf000,   0x2000
phy_init, data, phy,      0x11000,  0x1000
ota_0,    app,  ota_0,    0x20000,  0x400000   # 4 MB
ota_1,    app,  ota_1,    0x420000, 0x400000   # 4 MB
storage,  data, fat,      0x820000, 0x7E0000   # ~7.9 MB, reserved, no filesystem written
```

4 MB slots are a fourfold headroom over today's image. That is not padding for its own sake: the camera with
H.264, LVGL for an on-board display, and lidar all live in the same image, and hitting the slot ceiling halfway
would mean a wired reflash. `storage` is allocated now and left empty — unallocated flash cannot be claimed later
without changing the table, while an empty partition costs nothing at runtime. It is also where a slave image
would go if radio OTA is ever wanted, so that door stays open.

The subtype is a reservation, not a choice of filesystem: nothing is mounted or formatted, and whichever future
feature claims the space picks its own format then. The declared subtype only has to be one the partition-table
generator accepts.

**Versioning** keeps the current scheme: `v<semver>+<git rev-list --count HEAD>`, `PROJECT_VER` set before
`project()`. Cloning with history continues the count from ~461. Build numbers are only ever compared within one
car, so continuity across repos does not matter.

**Releases and OTA.** The contract with the app is unchanged: `POST /ota` with a raw `.bin`, the app compares the
build number, the launch gate force-updates a lagging board. Three details change:

- The artifact is `ajmiddlecar.bin`; `tools/release.sh` moves to the new layout
- The C6 slave image is **not** attached to releases — it is reproduced by building `firmware/c6/`
- The app currently picks the **first asset ending in `.bin`**. That works while a release holds one file but is a
  fragile heuristic; since the app is forking anyway, it changes to an exact filename match

OTA mechanics are untouched: rollback, marking the image valid at boot, the stall budget on receive, `car_stop()`
before flashing. IDF 6.0's new bootloader-OTA is not adopted.

## iOS app

The app is forked. Everything substantive — `DriveView`, tricks, the calibration wizard, dimensions, wheel
parameters, auto-return, the launch gate — moves untouched. Seven changes:

1. `project.yml`: product `AJMiddleCar`, bundle id `com.adamjohnson.ajmiddlecar` — a distinct id, otherwise the new
   app replaces the old one on the phone
2. **Different icon and accent colour** — two identical icons on the home screen guarantee driving the wrong car eventually
3. `UpdateClient`: releases point at AJMiddleCar; asset chosen by exact name instead of "first `.bin`"
4. The Firmware screen shows the radio version from the new `/status` field
5. App name in `L.swift`
6. `CarHost` — **unchanged**: both cars are a softAP at `192.168.4.1`
7. `tools/mock_car` is copied and serves the new `radio` field

**SSID must change.** The firmware currently hardcodes `ESP32-Car`. With two cars powered on nearby under the same
name, the phone would attach to whichever answers first. The middle car uses SSID **`AJMiddleCar`** with the
password unchanged (`drive1234`). The app never reads the SSID — it has no such entitlement and targets the gateway
address directly — so this only affects the human joining the network. It is the one place where "two parallel
cars" leaks into the code.

## Telling the two cars apart

Both cars are a softAP serving the same API at `192.168.4.1`. A different SSID is necessary but not sufficient:
join the wrong network — trivially easy when two are in range — and the pico app will find a car exactly where it
expects one. Today `/status` answers `"device":"esp32-car"` on both, and `CarStatus.bootstrap()` accepts any board
that returns that string. The wrong app would drive the wrong car with the wrong calibration, dimensions and
tricks, and nothing would warn anyone.

Three pieces close it:

1. The middle car's `/status` returns **`"device":"ajmiddlecar"`**
2. Each app checks for **its own** identifier and treats a mismatch as "not my car" — not as "offline"
3. On mismatch the app says so explicitly: a screen naming which car was found and which was expected, rather than
   a silent failure to connect that reads like a broken car

The app already fetches `/status` as its identity probe, so this costs one comparison and one screen. It must land
in the pico app too — a one-line change there, and without it the protection only works in one direction.

## Testing strategy

Four independent gates, only the last of which needs the board:

| Gate | Proves | Board needed |
|---|---|---|
| `cd firmware/p4/test && make run` | The pure modules survived the move: mixing, PWM planning, frame parsing, watchdog, recovery, NVS serialization | No |
| `idf.py build` for `esp32p4` on IDF 6.0.2 | Toolchain, major-version API breakage, the `esp_hosted` component, partition-table validity, image fits the slot | No |
| `xcodebuild` + simulator against the mock | The whole app flow: gate → connect → drive → settings → calibration | No |
| `docs/bringup.md` | Pins, a live SDIO link, motors actually turning | Yes |

This is what makes "verify the hardware last" affordable: several of the open assumptions below are settled by
**compiling**, with no board present. A wrong transport-init symbol fails to compile, and IDF 6.0 breakage surfaces
in the same build.

One caveat, stated precisely because it is tempting to overclaim: an `esp_wifi` call that `esp_wifi_remote` does not
support may fail at link time — or the component may provide a stub returning `ESP_ERR_NOT_SUPPORTED`, in which case
it builds and links cleanly and only misbehaves on the bench. Which of the two happens is itself unverified, so that
assumption is carried to bring-up rather than counted as closed.

## Open assumptions → `docs/bringup.md`

1. Which GPIOs are free for I2C, and which the SDIO link to the C6 occupies — *needs the board*
2. Whether the C6 arrives from the factory already flashed with the slave image; if so the flashing step collapses
   into a version check — *needs the board*
3. Whether `esp_wifi_remote` proxies `esp_wifi_ap_get_sta_list`, and whether an unsupported call fails loudly at
   link time or silently at runtime. If unsupported, the app's signal bars fall back to ping — already written —
   and driving is unaffected — *may surface at link time, otherwise needs the board*
4. The exact hosted transport-init function and its position in `app_main` — *settled by compiling*
5. Whether 5.4-era code builds on IDF 6.0.2 unchanged — *settled by compiling*
6. Whether the `esp_hosted` slave builds as a standalone project we own in `firmware/c6/`, or ships only as an
   example meant to be copied wholesale. This one is structural, not cosmetic: if it cannot be a thin project
   pinning the component, `firmware/c6/` takes a different shape — *settled by trying to build it*

## Out of scope

Camera/FPV, on-board display, lidar, battery monitoring, speed profiles, radio OTA, and any protocol change.
Feature parity with AJPicoCar is the finish line. Each of those gets its own spec, on a board that already drives.

## Risks

- **IDF 6.0 is a major release.** The port is expected to need fixes in a handful of places, but that is an
  estimate, not a measurement, and it is only tested when the build runs.
- **Hardware verified last, by choice.** Quarantined behind `board.h` and the bring-up checklist, but a genuinely
  surprising board (e.g. the SDIO link needing setup we have not anticipated) would surface late.
- **Two forked codebases.** Bugs common to both apps must be fixed twice. Accepted deliberately: the apps are
  expected to diverge once the P4 grows FPV and display screens. If the common surface proves stable instead, an
  SPM package can be extracted later.
