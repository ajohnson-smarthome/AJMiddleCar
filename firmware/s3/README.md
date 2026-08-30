# firmware/s3 — the USB-Ethernet dongle

An ESP32-S3 that plugs into an iPhone's USB-C port, presents itself as an Ethernet
adapter (CDC-NCM), and — from Plan 4 onwards — bridges that wire to a car's softAP.
The phone keeps its own Wi-Fi and cellular.

Design: `docs/research/2026-08-21-usb-ethernet-dongle.md`.

**This firmware knows nothing about any car.** No SSID, no protocol, no device id.
Like `firmware/c6/`, it is a modem. Everything car-shaped is told to it at runtime.

## The board

ESP32-S3 N16R8, u.FL, two Type-C. Silkscreen `ESP32-23 2022-V1.3` — a third-party
board on the DevKitC-1 pinout, not an Espressif one. The official DevKitC-1 has
Micro-USB on both ports and cannot reach an iPhone at all.

The two ports are silkscreened `USB` and `COM`, and they are not interchangeable:

| Question | Answer | Date |
|---|---|---|
| Which port is the UART bridge | `COM` — WCH CH343, `0x1A86:0x55D3` → `/dev/cu.wchusbserial5C840016191` | 2026-08-29 |
| Which port is native USB | `USB` — `0x303A:0x4001` (ROM bootloader, before this firmware is flashed), serial `123456` → `/dev/cu.usbmodem1234561` | 2026-08-29 |
| Powers from a C-to-C cable, both orientations | **yes** — Rd present on both CC lines (inferred from enumeration, not measured) | 2026-08-29 |
| macOS binds a CDC-NCM driver | **yes** — appears as `en11`, service "Espressif Device" | 2026-08-30 |
| Dongle's DHCP server configures the host | **yes** — host takes `192.168.7.2/24` | 2026-08-30 |
| Dongle answers on its own address | **yes** — `ping 192.168.7.1` 1.3 ms, 0% loss | 2026-08-30 |
| Host keeps its own default route | **yes** — `route get 1.1.1.1` unchanged; internet and DNS unaffected | 2026-08-30 |
| `GET /status` answers over the USB wire | **yes** — HTTP 200 in 12 ms | 2026-08-30 |
| iOS binds a CDC-NCM driver | **yes** — `http://192.168.7.1/status` (`:8080` today) answers in Safari on the phone | 2026-08-30 |
| iPhone keeps its own internet and DNS | **yes** — an ordinary site loads by name with the dongle attached | 2026-08-30 |
| `POST /net` persists across a reboot | *(record what you observed)* | |
| `GET /net` withholds the password | *(record what you observed)* | |
| `POST /ota` accepts an image and reboots into it | *(record what you observed)* | |
| The bootloader reverts an image that fails its first boot | *(record what you observed)* | |

The third row is the one that matters, and it was measured rather than assumed: a
USB-C source supplies VBUS only after it sees Rd, so enumeration over a C-to-C cable
*is* the CC test. Flipping the plug 180° tests the other CC line. Both orientations
enumerated, so an iPhone will power this board, and a future failure to appear on the
phone cannot be blamed on two missing resistors.

Detection is `ioreg -rc IOUSBHostDevice` and `ls /dev/cu.*`, not a power LED — the
shell sees this more reliably than a human squinting at the board.

Note that `USB` currently enumerates as a serial device, at the ROM bootloader's
`0x303A:0x4001`. It will stop doing so the moment TinyUSB claims the peripheral: on
ESP32-S3 the native USB pins are muxed between the USB-Serial-JTAG controller and
USB-OTG, and only one may hold them. A `USB` port that has gone quiet after flashing
is the expected outcome, not a fault — and once this firmware is running, that same
port answers as TinyUSB's default `0x303A:0x4000` instead, not the reading recorded
above.

## The dongle must never advertise itself as a gateway

Learned the hard way on 2026-08-30: an early build set the interface's `gw` to its own address, so
the DHCP server sent the **router option**. macOS ranks a wired service above Wi-Fi, installed a
default route through the dongle, and posted every packet to a device with no uplink. The host lost
the internet outright.

The firmware now advertises no router (`esp_netif_dhcps_option` with
`ESP_NETIF_ROUTER_SOLICITATION_ADDRESS`, plus `gw = 0.0.0.0`), and this is verified rather than
assumed: `ipconfig getpacket en11` shows no `router` option, and `route get 1.1.1.1` is byte-identical
with the dongle attached and detached.

One thing cannot be fixed and does not need to be: IDF's DHCP server emits `domain_name_server` on
every code path, advertising the dongle's own address when none is configured
(`dhcpserver.c:502-506`). Measured effect on a real host: none — names resolve in 0.13 s with the
dongle attached, because the host's own resolvers stay primary while we are not its default route.

**A caveat for anyone debugging this.** A VPN client can re-bind its outer socket to the dongle the
moment it appears, on the "wired beats Wi-Fi" heuristic, and then quietly fail to hand-shake through a
device with no uplink — which looks exactly like the dongle stealing traffic. Tell them apart with
`route get 1.1.1.1` (the routing decision itself) and `netstat -ibn -I en11` (a client retrying into
the void shows lopsided counters and a packet every ~5 s). One client did this; another did not.

## The question this firmware existed to ask

**iOS accepts a class-compliant CDC-NCM device.** Confirmed on hardware 2026-08-30: the dongle
plugged into an iPhone's USB-C port, and `http://192.168.7.1/status` (`:8080` today) answered in Safari.

That one request proves the entire chain at once — iOS enumerated the device, bound a CDC-NCM
driver to it, accepted an address from the dongle's own DHCP server, routed a TCP connection over
the wired interface, and got a reply. No app, no profile, no MFi, no entitlement: the phone treats
it as an ordinary network interface, which is exactly what the design asked for.

Nothing in Espressif's documentation promised this. IDF's `tusb_ncm` example names Linux and
Windows only, and iOS appears nowhere in it — which is why this plan was cut to the smallest slice
that could ask the question, and why everything downstream was gated on the answer.

**And the phone kept its own internet.** An ordinary site loaded by name with the dongle attached —
which is the premise the whole project rests on, and the one an early build broke on the Mac at the
cost of a session's connectivity. It also closes the DNS question from the other direction: the
dongle hands out its own address as a resolver in every lease and IDF offers no way to stop it
(`dhcpserver.c:502-506`), but a host whose default route we do not take keeps its own resolvers
primary. Measured now on both macOS and iOS rather than argued.

The answer opens the rest: the config channel, the radio, and the proxy to the car.

## The Mac acceptance run

Everything Plan 1 promised, measured in one pass on 2026-08-30. The reply below names the actual
binary, and it is not `94f81ae`: `g51f1f8e-dirty` is `94f81ae`'s *parent* commit, built dirty — this
run exercised a tree one commit behind the `GET /status` commit it was meant to be checking, not
that commit's own content. Anyone rebuilding at `94f81ae` should see a clean `g94f81ae` in its
place, not this string. `"idf":"v6.0.2-dirty"` records the same kind of gap one layer down: the
local ESP-IDF checkout also carried uncommitted changes at build time.

```
GET /status:
  {"dev":"ajdongle","fw":"v1.0+483-147-g51f1f8e-dirty","idf":"v6.0.2-dirty","usb":"up"}
    [HTTP 200 in 0.012553s]
```

The records above are from the run of 2026-08-30, when the dongle served `:80` and named
itself with the key `dev`. This commit moves the server to `:8080` and renames that key to
`device`; the observations are left as they were taken, and the next bench run will record
the new shape. Port 80 moved off, not away: it stays reserved for the car, which a later
plan forwards through the dongle untouched, so that the car's own contract and the app's
`CarHost.port` never have to move.

| Check | Baseline (no dongle) | Dongle attached |
|---|---|---|
| `route get 1.1.1.1` | `utun4` | `utun4` — unchanged |
| internet by IP | 0.103 s | 0.107 s |
| internet by name (needs DNS) | 0.592 s | 0.136 s — a warm DNS cache on the second run, not evidence the dongle helped |
| Wi-Fi latency to the LAN router | 13.6 ms avg | 9.2 ms avg — ordinary jitter between two pings, not evidence the dongle helped |
| DHCP options received | — | 10 options, **no `router`** |
| dongle interface counters | — | 13 in / 68 out — a quiet device |

The host is measurably no worse off for having the dongle plugged in, which is the whole premise —
and the strong evidence for that is the unchanged routing decision and the absent `router` option,
not the two rows above that merely happened to come back faster.
The script that produces this, `firmware/s3/verify-on-host.sh`, is checked in for Plan 2: it checks
the deliverable and the regression in the same run, so a returning route capture is caught by a test
rather than by losing somebody's connectivity.

## Bench

Measurements taken as each plan lands, rather than assumed.

| Check | Result | Date |
|---|---|---|
| Built image size with the station radio (`esp_wifi` + the join state machine) | 870 KB (`ajdongle.bin`, 890,752 bytes / 0xd9780) in a 4 MB OTA slot — 21% used, 3.15 MB free | 2026-08-30 |

The app was 395 KB before this plan added the radio; roughly double, as expected, and comfortable
against the slot — the earlier 1 MB partition this project considered and discarded would have been
tight.

## Build

```bash
source tools/env-p4.sh        # the IDF export script is target-agnostic; the target
                              # comes from sdkconfig.defaults, not from the environment
cd firmware/s3 && idf.py build
idf.py -p /dev/cu.wchusbserial5C840016191 flash monitor
```

**First build on a fresh machine needs the Xtensa toolchain.** The car and its radio
are both RISC-V targets, so an ESP-IDF installed for this project alone has no Xtensa
compiler and the S3 build fails on a missing toolchain. Install it once:

```bash
~/esp/esp-idf-v6.0.2/install.sh esp32s3
```

The console is on UART0, reached through the `COM` port — not on the native USB,
because TinyUSB owns that peripheral.

## Updating over USB

Once the dongle is running an image with `/ota`, the cable is only needed for the first flash:

```bash
cd firmware/s3 && idf.py build
curl --data-binary @build/ajdongle.bin \
     -H 'Content-Type: application/octet-stream' \
     http://192.168.7.1:8080/ota
```

Expect `{"ok":true}`, then the USB interface drops and comes back within a few seconds as the
dongle reboots into the new slot. Confirm with `/status`:

```bash
curl -s http://192.168.7.1:8080/status
```

`fw` should be the version just built, and **`rollback` should be `false`**. `rollback:true` means
the bootloader put the previous image back — the new one failed its first boot before `app_main`
finished, so it never got to cancel the revert. That is the safety net working, not a bug in the
update; the `fw` you see is the old image, and pushing the same binary again will do the same thing.

**Rollback protects against an image that panics, not one that hangs.** A panic reboots
immediately and the bootloader reverts on the next boot, unaided. An image that instead hangs
without panicking never reboots, so the bootloader never gets that chance — recovering from one
needs nothing more than unplugging and replugging the dongle. Trivial for a device you are
already holding, but worth knowing before mistaking a silent dongle for a bricked one.

`/ota` is unauthenticated: anything that can reach `192.168.7.1:8080` can push a new image —
today that is only the host on the other end of the USB cable, which could already reflash the
dongle by cable anyway.
