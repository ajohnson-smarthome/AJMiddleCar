# firmware/s3 — the USB-Ethernet dongle

An ESP32-S3 that plugs into an iPhone's USB-C port, presents itself as an Ethernet
adapter (CDC-NCM), and — from Plan 3 onwards — bridges that wire to a car's softAP.
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
| Which port is native USB | `USB` — `0x303A:0x4001`, serial `123456` → `/dev/cu.usbmodem1234561` | 2026-08-29 |
| Powers from a C-to-C cable, both orientations | **yes** — 5.1 kΩ present on CC1 and CC2 | 2026-08-29 |
| macOS binds a CDC-NCM driver | **yes** — appears as `en11`, service "Espressif Device" | 2026-08-30 |
| Dongle's DHCP server configures the host | **yes** — host takes `192.168.7.2/24` | 2026-08-30 |
| Dongle answers on its own address | **yes** — `ping 192.168.7.1` 1.3 ms, 0% loss | 2026-08-30 |
| Host keeps its own default route | **yes** — `route get 1.1.1.1` unchanged; internet and DNS unaffected | 2026-08-30 |
| `GET /status` answers over the USB wire | **yes** — HTTP 200 in 12 ms | 2026-08-30 |
| iOS binds a CDC-NCM driver | *(Task 5 — the question this whole plan exists to answer)* | |

The third row is the one that matters, and it was measured rather than assumed: a
USB-C source supplies VBUS only after it sees Rd, so enumeration over a C-to-C cable
*is* the CC test. Flipping the plug 180° tests the other CC line. Both orientations
enumerated, so an iPhone will power this board, and a future failure to appear on the
phone cannot be blamed on two missing resistors.

Detection is `ioreg -rc IOUSBHostDevice` and `ls /dev/cu.*`, not a power LED — the
shell sees this more reliably than a human squinting at the board.

Note that `USB` currently enumerates as a serial device. It will stop doing so the
moment TinyUSB claims the peripheral: on ESP32-S3 the native USB pins are muxed
between the USB-Serial-JTAG controller and USB-OTG, and only one may hold them. A
`USB` port that has gone quiet after flashing is the expected outcome, not a fault.

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

## The Mac acceptance run

Everything Plan 1 promised, measured in one pass on 2026-08-30 against build `94f81ae`:

```
GET /status:
  {"dev":"ajdongle","fw":"v1.0+483-147-g51f1f8e-dirty","idf":"v6.0.2-dirty","usb":"up"}
    [HTTP 200 in 0.012553s]
```

| Check | Baseline (no dongle) | Dongle attached |
|---|---|---|
| `route get 1.1.1.1` | `utun4` | `utun4` — unchanged |
| internet by IP | 0.103 s | 0.107 s |
| internet by name (needs DNS) | 0.592 s | 0.136 s |
| Wi-Fi latency to the LAN router | 13.6 ms avg | 9.2 ms avg |
| DHCP options received | — | 10 options, **no `router`** |
| dongle interface counters | — | 13 in / 68 out — a quiet device |

The host is measurably no worse off for having the dongle plugged in, which is the whole premise.
The script that produces this is worth keeping for Plan 2: it checks the deliverable and the
regression in the same run, so a returning route capture is caught by a test rather than by losing
somebody's connectivity.

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
