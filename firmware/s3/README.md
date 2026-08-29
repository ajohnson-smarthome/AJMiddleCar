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
