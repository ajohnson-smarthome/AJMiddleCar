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
| Built image size with the station radio (`esp_wifi` + the join state machine), SoftAP support compiled out (`CONFIG_ESP_WIFI_SOFTAP_SUPPORT=n`) | 819 KB (`ajdongle.bin`, 838,592 bytes / 0xccbc0) in a 4 MB OTA slot — 20% used, 3.20 MB free | 2026-08-30 |
| `curl http://<dongle's station address>:8080/status` from a machine on the car's network — must be refused (`api_guard.c`'s `open_fn`, checked by `getsockname` against `DONGLE_HOST`) | *(needs a car and a second machine on its network — the only test of the guard; record what you observed)* | |
| Which `NWInterface.InterfaceType` iOS reports for the dongle's CDC-NCM interface (U1) | **Not `.wiredEthernet`** — that pin could not open a socket to a dongle the phone was demonstrably talking to. Which type it *is* was never established, because the question was removed rather than answered: `CarInterface` now finds the wire by the address it carries. See the write-up below | 2026-08-31 |
| The phone enumerates the dongle and gets an address | `esp_netif_lwip: DHCP server assigned IP to a client, IP is: 192.168.7.2` — iOS brought up CDC-NCM unaided, and the dongle drew enough from the phone alone | 2026-08-31 |
| Safari on the phone → `192.168.7.1:8080/status` and `192.168.7.1/status` | Both answered — the dongle and, through the relay, the car. This is what proved the fault was the app's and not the firmware's, and it cost no build at all | 2026-08-31 |
| The app talks to the car through the relay, on a device, with no `-carHost` argument | Works. The escape hatch is kept for now anyway — see below | 2026-08-31 |
| The station joins the car and keeps its credentials across a reboot (`/net` in NVS) | `joined: ip=192.168.4.2 gw=192.168.4.1`, and it rejoined by itself after a cable reflash with no second `POST /net` | 2026-08-31 |
| `tools/conformance.py http://192.168.7.1` — the car's whole REST surface, relayed | Every endpoint passed except one pre-existing car-vs-mock divergence, unrelated to the relay: `POST /calib/save` with string `pair`s is correctly rejected `400`, but the car's envelope names `field:"pair"` where the mock and the test expect `"wheels"`. `docs/protocol.md` says `field` names *the offending key*, so the car is right and the mock and `conformance.py` are the ones to correct. First time this suite had ever run against real hardware | 2026-08-31 |
| `tools/conformance_rt.py 192.168.7.1:4210` — the real-time channel, relayed | All checks passed: hello, wrong-proto rejection, telemetry, datagram drops measured by `rx_fps`, the session cap, eviction, `bye`. The Mac has no route to `192.168.4.1`, so every one of those frames went through the dongle | 2026-08-31 |
| Relayed `GET /status` round trip | 0.489 s cold (Wi-Fi association plus the upstream connect), well inside the app's budget | 2026-08-31 |
| The startup sequence, on a device, end to end | Walked all six steps and reached the drive screen. First time any of it has executed outside the gallery: the phases, the 400 ms pacing, the new art and the strict version gate had all been built and none had run | 2026-08-31 |
| `searching` — the radio telling "no car on the air" from "cannot connect" | Executes. `wifi_sta: associated; waiting for an address` fires between the join request and the lease, and the window is real: 3572 ms to association, 4942 ms to an address. Step 4 has something to show | 2026-08-31 |
| The strict version gate, with both devices current | Satisfied and passes. Both flashed by cable from the release's own images, so nothing was asked to update — which is the point: the gate lets a current pair through without an OTA | 2026-08-31 |
| A release carrying the adapter's image | `v1.0+725` is the first one. Every release before it held only `ajmiddlecar.bin`, so the adapter's release lookup returned nothing — invisible until the gate became strict, at which point the app locked itself out and blamed the network | 2026-08-31 |

The app was 395 KB before this plan added the radio; roughly double, as expected, and comfortable
against the slot — the earlier 1 MB partition this project considered and discarded would have been
tight.

### The app's turn: a guess that could not be tested off a device

The firmware worked and the app still said "no adapter" with the cable plugged in. The app pinned
every socket to an interface *type* the spec assumed iOS reports for CDC-NCM (`.wiredEthernet`).
Ruling that out cost no builds at all, which is the part worth copying: the dongle's own console
showed the phone taking a DHCP lease, and **Safari on the same phone loaded both the dongle's
`/status` and the car's through the relay**. Two free observations, and everything below the app
was exonerated — leaving exactly one difference between Safari and us, the pin.

The repair was to stop asking the question. `app/AJMiddleCar/CarInterface.swift` defines the
dongle's wire as *the interface holding an address on the dongle's subnet* — a subnet this project
owns end to end, since `DongleContract.host` is generated from the contract and the dongle's own
DHCP server hands the phone the neighbouring address. Whatever iOS calls that interface, it is
found by the address on it. `CarNet` pins to that concrete `NWInterface`, or leaves the socket
unpinned when Network.framework does not offer the wire at all — legitimate here, because the
dongle advertises neither gateway nor DNS by design, and routing still delivers on-link.

The general lesson: **an assumption that cannot be tested without the hardware it is about will
survive every review and fail on the bench.** The type-based pin was reviewed repeatedly and
recorded as a known unknown (U1) — which did not help, because a known unknown is still an
unknown. Replacing it with a definition that any machine can check (the host test finds `lo0` by
asking for `127.0.0.0/24`) is what actually retired it.

### What the bench caught that no host test could

The relays did not work on first power-up, and the way they failed is worth keeping. The dongle
looked entirely healthy — associated, addressed by the car's DHCP, both relays bound and logging,
its own `/status` answering in milliseconds — while **every** packet either relay sent toward the
car failed with `errno 12` (`ENOMEM`) and TCP connects timed out after five seconds. The heap was
not exhausted (230 KB internal free, a 152 KB largest block) and routing was not confused
(`ip4_route(192.168.4.1)` returned the station netif, address `192.168.4.2`). The refusal came
from below lwIP: with PSRAM enabled and `CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER`, `esp_wifi_internal_tx`
could not obtain a DMA-capable buffer, `wlanif`'s `low_level_output` returned `ERR_MEM`, and the
socket layer surfaced that as `ENOMEM`. Switching to `CONFIG_ESP_WIFI_STATIC_TX_BUFFER` fixed it
outright — see the comment at that line in `sdkconfig.defaults`, which now records this so nobody
switches it back. The lesson for the next radio-bearing board in this project: with PSRAM on,
static TX buffers are not a tuning preference, they are a requirement.

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
