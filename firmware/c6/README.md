# The radio

The ESP32-P4 has no radio. On this board an **ESP32-C6 sits beside it on SDIO** and does all the
WiFi. This directory is that co-processor's home. It contains no source, and that is the point.

## What runs on the C6

Espressif's `esp_hosted` co-processor firmware — a network adapter. It knows nothing about this
car: not the motors, not the protocol, not `/ws`. It carries 802.11 frames between the radio and
the P4 over SDIO, and that is all. The P4 calls the ordinary `esp_wifi` API; `esp_wifi_remote`
marshals those calls across.

**We do not author this image.** It is built from the exact `esp_hosted` version the host is
pinned to in `firmware/p4/main/idf_component.yml` — currently **3.0.6** — using the project that
ships inside that component. Host and co-processor versions must match: `/status` reports what
the C6 actually runs, `board.h` records what this firmware expects, and a mismatch shows up as
`radio.ok: false` rather than as WiFi behaving strangely for no visible reason.

There is no `CMakeLists.txt` here because there is nothing of ours to build. The image comes from
`firmware/p4/managed_components/espressif__esp_hosted/examples/wifi/sta/cp`, which arrives with
the host build and is not in git — `dependencies.lock` pins it, which is the actual guarantee.

## Building and flashing

```bash
firmware/c6/flash-radio.sh                        # build only
firmware/c6/flash-radio.sh /dev/cu.usbserial-XXX  # build and flash
```

Flashing goes through the board's **ESP32-C6 UART header**, not through the P4's USB port. If the
flash fails because the host is holding the bus, put the P4 into bootloader mode first:

```bash
esptool.py -p <P4_PORT> --before default_reset --after no_reset run
```

### The ESP-IDF patch

The co-processor's SDIO datapath uses software aggregation — several frames packed into one
transfer — which sends more than stock ESP-IDF's 4092-byte cap allows. `flash-radio.sh` applies
the vendor's own patch (`eh.py patch-idf`) automatically, and it is idempotent.

**This means `~/esp/esp-idf-v6.0.2` is not a stock checkout.** Reinstall ESP-IDF and the radio
build stops with an explicit error telling you to re-patch; run the script again and it does.

The alternative was the `PACKET` or `STREAM` datapath, neither of which needs the patch. They
were not chosen: throughput is irrelevant to a car that sends 24-byte frames ten times a second,
but aggregation is the configuration the vendor tests, and the radio link is precisely where an
untested deviation would be most expensive to debug — especially since it will matter later, when
video shares this bus.

## Wiring

The two chips number their own pins, so the tables differ. The P4 side is pinned in
`firmware/p4/sdkconfig.defaults`; the C6 side is fixed by its SDIO slave peripheral.

| Signal | P4 (host) | C6 (co-processor) |
|---|---|---|
| CLK | 18 | 19 |
| CMD | 19 | 18 |
| D0 | 14 | 20 |
| D1 | 15 | 21 |
| D2 | 16 | 22 |
| D3 | 17 | 23 |
| Reset | 54 → | EN/RST |

Those P4 GPIOs are unavailable to anything else — check here before assigning a pin in `board.h`.

**External 51 kΩ pull-ups on `CMD` and `D0`–`D3` are mandatory** per Espressif, irrespective of
jumpers. On an integrated board like this one they should already be on the PCB; confirm it at
bring-up, because pull-ups missing on `D2`/`D3` also let the slave fall into SPI mode at startup.

## Checking it worked

`GET /status` on the car:

```json
"radio": {"fw": "3.0.6", "expected": "3.0.6", "ok": true}
```

The app shows the same line on its Firmware screen.
