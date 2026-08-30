# Dongle — Plan 1: the NCM device and its own address

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A new `firmware/s3/` project that enumerates on a USB host as an Ethernet interface, hands that host an address by DHCP, answers ping, and serves `GET /status` — proven on macOS first and on an iPhone second.

**Architecture:** `esp_tinyusb`'s NCM class provides the USB wire. Unlike IDF's `tusb_ncm` example — which is a transparent L2 bridge with no IP stack on the USB side — this firmware attaches its **own `esp_netif`** to that wire, with a static address and a DHCP server. That is what makes the dongle an endpoint the app can talk to before any radio exists, which the config channel in Plan 2 depends on. No Wi-Fi in this plan at all.

**Tech Stack:** C11, ESP-IDF 6.0.2, `espressif/esp_tinyusb ^2.0.1~1`, `esp_netif` + lwIP, `esp_http_server`. Target `esp32s3`.

**Spec:** `docs/research/2026-08-21-usb-ethernet-dongle.md` — the design note this plan implements. Read it first; the reasoning behind every choice below lives there and is not repeated here.

## Global Constraints

- **The dongle knows nothing about any car.** No SSID, no password, no `device_id`, no protocol. It is a modem, not a brain — the same rule `firmware/c6/` lives under. Anything car-shaped that appears in `firmware/s3/` is a bug in this plan, not a feature.
- **`firmware/s3/` does not reference `app/` or `firmware/p4/`,** and neither references it.
- USB-side network: **`192.168.7.1/24`**, dongle at `.1`, DHCP pool starts at `.2`. Chosen to miss the common home ranges (`192.168.0.x`, `192.168.1.x`, `10.x`).
- Device identity string: **`ajdongle`**. Project name: `ajdongle`.
- **The console lives on UART0**, reached through the board's second Type-C via its bridge chip. The native USB belongs to TinyUSB and cannot also be USB-Serial-JTAG — on ESP32-S3 GPIO19/20 are muxed between the two controllers, one at a time.
- Board in hand: ESP32-S3 **N16R8** (16 MB flash, 8 MB octal PSRAM) with u.FL, two Type-C ports, silkscreen `ESP32-23 2022-V1.3`. Not an official Espressif DevKitC-1 — its USB-C wiring is the vendor's own, which is why Task 1 exists.
- `tools/test-all.sh` must stay green after every task. It does not build this firmware (it builds no firmware), so "green" here means "not broken by the new directory".

## Why this plan stops where it does

The dongle is a **standalone device**, not a length of wire with firmware on it: it
holds its own configuration, describes itself over an API, and only then proxies for
something else. That shape decides the order — everything that can be exercised with
nothing but a laptop and `curl` comes before anything that needs a car.

| Plan | Deliverable | Needs | Gated on |
|---|---|---|---|
| **1 (this one)** | NCM device with its own IP and `GET /status` | a host | nothing |
| 2 | The config domain: `GET`/`POST /net`, NVS, host-tested validation | a host | Plan 1 answering yes on iPhone |
| 3 | Radio: join the configured AP, report it in `/status`, proxy to the car | a car | Plan 2 |
| 4 | App side: `.wiredEthernet`, `CarHost`, the new link states | a phone | Plan 3 |

Plan 2 before Plan 3 is the point of the reordering: the whole configuration surface —
set an AP, read back what it is set to, see whether it is connected — is finished and
verifiable before a radio is ever switched on. A dongle that has never seen a car is
still a fully testable device.

Everything after Plan 1 assumes iOS accepts a class-compliant CDC-NCM device, and **nobody has established that.** The `tusb_ncm` README names Linux and Windows only; iOS is not mentioned. Building the bridge before that question is answered risks building it for nothing, so this plan answers it with the smallest surface that still produces code we keep.

## A note on testing

This plan has **no host tests**, and that is not an oversight. Every line in it is ESP-IDF glue — driver installs, netif attachment, an HTTP handler. There is no pure logic to test, and unit tests around `tinyusb_driver_install` would be theatre. The deliverables are verified on the bench instead, and each task says exactly what to observe.

Pure logic arrives in Plan 2 (config parsing and validation) and gets host tests under `firmware/s3/test/` then, following `firmware/p4/test/`'s pattern.

## File Structure

| File | Responsibility |
|---|---|
| `firmware/s3/CMakeLists.txt` | Project definition. No version-from-git machinery yet — that belongs with OTA, which the dongle does not have |
| `firmware/s3/sdkconfig.defaults` | Target, NCM mode, console on UART0, flash and PSRAM for N16R8. Every line commented with *why*, as in `firmware/p4/sdkconfig.defaults` |
| `firmware/s3/main/CMakeLists.txt` | Component registration |
| `firmware/s3/main/idf_component.yml` | `espressif/esp_tinyusb` dependency |
| `firmware/s3/main/main.c` | `app_main`: brings up NCM, then the netif, then the HTTP server |
| `firmware/s3/main/usb_net.{c,h}` | The seam between TinyUSB's frame callbacks and `esp_netif`. The only file that knows both sides exist |
| `firmware/s3/main/status_api.{c,h}` | The `GET /status` handler and the HTTP server it registers on |
| `firmware/s3/README.md` | What this board is, how to build and flash it, and the live record of what the hardware has answered |
| `.gitignore` | `firmware/s3/build/`, `firmware/s3/managed_components/` |
| `CLAUDE.md` | One line in the Layout section |

---

### Task 1: Bench pre-flight — is this board a compliant USB-C device?

No code. Five minutes with a cable that prevents a week of misdiagnosis: if the board lacks CC pull-downs, an iPhone will not power it, and that failure is indistinguishable from "iOS refuses NCM" unless you have ruled it out first.

**Files:**
- Create: `firmware/s3/README.md`

- [ ] **Step 1: Identify which Type-C port is which**

Plug each port into a Mac in turn and run `ls /dev/cu.*` before and after.

- The port that makes a `/dev/cu.usbserial-*` or `/dev/cu.wchusbserial*` appear is the **UART bridge** — this is where the console lives, and where `idf.py flash monitor` goes.
- The port that appears only when the chip is in download mode (hold BOOT, tap RST) as `/dev/cu.usbmodem*` is the **native USB** — this is the one that goes to the iPhone.

Record both in the README.

- [ ] **Step 2: Test the native USB port with a C-to-C cable**

Connect the native-USB port to a USB-C charger or a Mac **with a C-to-C cable** — not through a USB-A adapter, because only a Type-C source tests the CC lines.

Then flip the plug 180° and repeat.

| Observation | Meaning | Consequence |
|---|---|---|
| Powers in both orientations | 5.1 kΩ on CC1 and CC2 | iPhone will power it; proceed |
| Powers in one orientation only | one CC resistor | works, but the plug has a "right way up" |
| No power on C-to-C, powers on A-to-C | no CC resistors | **iPhone will not power it** — see step 3 |

- [ ] **Step 3: If there are no CC resistors, choose a workaround before continuing**

Either solder 5.1 kΩ from CC1 and CC2 to GND, or plan to run the iPhone test through a USB-C→USB-A(female) adapter plus an A→C cable — USB-A semantics supply VBUS unconditionally, bypassing CC negotiation entirely.

Do not skip this decision and discover it during the iPhone test.

- [ ] **Step 4: Write the README with what the board answered**

```markdown
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

| Question | Answer | Date |
|---|---|---|
| Which port is the UART bridge | *(record in step 1)* | |
| Which port is native USB | *(record in step 1)* | |
| Powers from a C-to-C cable, both orientations | *(record in step 2)* | |

## Build

```bash
source tools/env-p4.sh        # the IDF export script is target-agnostic; the target
                              # comes from sdkconfig.defaults, not from the environment
cd firmware/s3 && idf.py build
idf.py -p /dev/cu.<uart-bridge-port> flash monitor
```

The console is on UART0, not on the native USB — TinyUSB owns that peripheral.
```

Replace each italic placeholder with what you actually observed. A row you did not
test keeps its placeholder; an untested row must never read as a pass.

- [ ] **Step 5: Commit**

```bash
git add firmware/s3/README.md
git commit -m "docs(s3): what the dongle board answered on the bench"
```

---

### Task 2: The project skeleton, and NCM enumerates

**Files:**
- Create: `firmware/s3/CMakeLists.txt`
- Create: `firmware/s3/sdkconfig.defaults`
- Create: `firmware/s3/main/CMakeLists.txt`
- Create: `firmware/s3/main/idf_component.yml`
- Create: `firmware/s3/main/main.c`
- Modify: `.gitignore`
- Modify: `CLAUDE.md`

**Interfaces:**
- Produces: an `app_main` that brings up the NCM class. Task 3 replaces its frame callback with the netif seam.

- [ ] **Step 1: Write the project files**

`firmware/s3/CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ajdongle)
```

`firmware/s3/sdkconfig.defaults`:

```
CONFIG_IDF_TARGET="esp32s3"

# The USB device class. NCM rather than ECM or RNDIS because it is the one both
# macOS and iOS are expected to accept class-compliant, and the only mode IDF's
# tusb_ncm example is built around.
CONFIG_TINYUSB_NET_MODE_NCM=y

# Console on UART0, reached through the board's second Type-C and its bridge chip.
#
# This is forced, not preferred: on ESP32-S3 the native USB pins (GPIO19/20) are muxed
# between the USB-Serial-JTAG controller and the USB-OTG controller, and only one may
# have them. TinyUSB needs USB-OTG, so the moment NCM comes up the USB console would
# disappear mid-session. Putting the console on UART0 from the start means logs never
# move.
CONFIG_ESP_CONSOLE_UART_DEFAULT=y
CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=n

# N16R8: 16 MB flash, 8 MB PSRAM on an octal bus. Octal PSRAM occupies GPIO33-37 —
# nothing here needs them, but note it before assigning pins.
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
```

`firmware/s3/main/CMakeLists.txt`:

```cmake
idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES nvs_flash)
```

`firmware/s3/main/idf_component.yml`:

```yaml
## Pinned to the same major the IDF 6.0.2 tusb_ncm example uses.
dependencies:
  espressif/esp_tinyusb:
    version: "^2.0.1~1"
```

- [ ] **Step 2: Write `main.c` — NCM and nothing else**

```c
#include "esp_err.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs_flash.h"

#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_net.h"

static const char *TAG = "dongle";

/* Stage 1: the wire exists and nothing consumes it yet. Task 3 hands these frames
 * to lwIP. Returning ESP_OK without reading the buffer is a deliberate discard, not
 * an unfinished path. */
static esp_err_t on_usb_frame(void *buffer, uint16_t len, void *ctx)
{
    (void)buffer;
    (void)ctx;
    ESP_LOGD(TAG, "rx %u bytes from host", (unsigned)len);
    return ESP_OK;
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    const tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));

    tinyusb_net_config_t net_cfg = {
        .on_recv_callback = on_usb_frame,
    };
    /* The NCM interface needs a MAC. The station MAC is a real, per-device address
     * from the efuse block — borrowing it here costs nothing, because this firmware
     * never brings up a station in Plan 1. Plan 3 must revisit this: a station and
     * a USB interface on the same MAC would be two interfaces claiming one address. */
    ESP_ERROR_CHECK(esp_read_mac(net_cfg.mac_addr, ESP_MAC_WIFI_STA));
    ESP_ERROR_CHECK(tinyusb_net_init(&net_cfg));

    ESP_LOGI(TAG, "NCM up: %02x:%02x:%02x:%02x:%02x:%02x",
             net_cfg.mac_addr[0], net_cfg.mac_addr[1], net_cfg.mac_addr[2],
             net_cfg.mac_addr[3], net_cfg.mac_addr[4], net_cfg.mac_addr[5]);
}
```

- [ ] **Step 3: Add the ignores and the layout line**

Append to `.gitignore`, next to the existing `firmware/p4/build/` and `firmware/c6/build/` entries:

```
firmware/s3/build/
firmware/s3/managed_components/
```

In `CLAUDE.md`, in the Layout code block, add after the `firmware/c6/` line:

```
firmware/s3/   the USB-Ethernet dongle — knows nothing about the car
```

- [ ] **Step 4: Build**

```bash
source tools/env-p4.sh
cd firmware/s3 && idf.py build
```

Expected: the component manager fetches `espressif/esp_tinyusb`, and the build succeeds.
A failure naming `tinyusb_default_config.h` means the fetched component is older than
2.0.1 — check `dependencies.lock`.

- [ ] **Step 5: Flash and verify enumeration on the Mac**

Flash over the **UART bridge** port, then connect the **native USB** port to the Mac
with a C-to-C cable.

```bash
idf.py -p /dev/cu.<uart-bridge-port> flash monitor
```

Expected in the monitor: `NCM up: <mac>`.

Expected on the Mac — a new interface appears:

```bash
ifconfig | grep -A3 "^en"
```

Look for an `en*` interface that was not there before, and for the dongle in
System Settings → Network. It will have no usable address yet — that is Task 3.
What matters here is that the interface **exists**: macOS enumerated the device
and bound its NCM driver.

If no interface appears, stop and diagnose before continuing. Check the monitor for
a TinyUSB error, confirm the cable is data-capable, and confirm from Task 1 that
this port is the native USB one.

- [ ] **Step 6: Commit**

```bash
git add firmware/s3 .gitignore CLAUDE.md
git commit -m "feat(s3): the dongle enumerates as a USB NCM device"
```

---

### Task 3: The dongle gets its own address, and hands one to the host

The example this is derived from bridges frames straight into Wi-Fi and never builds
an IP stack on the USB side. We need the opposite: the dongle must be reachable *as
itself*, before any radio exists, because Plan 2's config channel is what tells it
which car to join.

**Files:**
- Create: `firmware/s3/main/usb_net.h`
- Create: `firmware/s3/main/usb_net.c`
- Modify: `firmware/s3/main/main.c`
- Modify: `firmware/s3/main/CMakeLists.txt`

**Interfaces:**
- Consumes: `tinyusb_net_init` from Task 2.
- Produces: `esp_err_t usb_net_start(void)` — installs the NCM class *and* the netif behind it. `esp_netif_t *usb_net_netif(void)` for later plans. After this call the dongle answers on `192.168.7.1`.

- [ ] **Step 1: Confirm the netif driver API against the fetched headers**

Before writing the glue, read what the build actually downloaded:

```bash
grep -n "esp_netif_set_driver_config\|driver_free_rx_buffer\|post_attach" \
  $IDF_PATH/components/esp_netif/include/esp_netif_defaults.h \
  $IDF_PATH/components/esp_netif/include/esp_netif_types.h
grep -rn "tinyusb_net_send_sync" \
  firmware/s3/managed_components/espressif__esp_tinyusb/include/
```

The signatures below are written against IDF 6.0.2's documented custom-I/O-driver
pattern. If a name differs in the headers, the header wins — fix the code, and note
the difference in a comment so the next reader does not re-derive it.

- [ ] **Step 2: Write `usb_net.h`**

```c
#ifndef USB_NET_H
#define USB_NET_H

#include "esp_err.h"
#include "esp_netif.h"

/* Brings up the USB NCM class and attaches an lwIP interface to it.
 *
 * The dongle is an endpoint on this wire, not a transparent bridge: it holds
 * USB_NET_ADDR and runs a DHCP server, so a host that plugs in is configured with
 * no help from anything else. That property is what lets the app reach the dongle
 * before the dongle has joined any network — see the design note. */
esp_err_t usb_net_start(void);

/* The interface, for later plans that need to bridge or route through it. */
esp_netif_t *usb_net_netif(void);

#define USB_NET_ADDR "192.168.7.1"
#define USB_NET_MASK "255.255.255.0"

#endif /* USB_NET_H */
```

- [ ] **Step 3: Write `usb_net.c`**

```c
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"

#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_net.h"

#include "usb_net.h"

static const char *TAG = "usb_net";

static esp_netif_t *s_netif;

/* lwIP has a frame for the host. */
static esp_err_t usb_transmit(void *h, void *buffer, size_t len)
{
    (void)h;
    return tinyusb_net_send_sync(buffer, len, NULL, pdMS_TO_TICKS(100));
}

/* lwIP is done with a frame we handed it in on_usb_frame. That frame is our copy. */
static void usb_free_rx(void *h, void *buffer)
{
    (void)h;
    free(buffer);
}

/* The host has a frame for us.
 *
 * TinyUSB's buffer is only valid for the duration of this callback, so the frame is
 * copied before it goes to lwIP, which keeps it until usb_free_rx. Copying every
 * frame is the honest version; if it ever shows up in a profile, the fix is a pool,
 * not a borrowed pointer. */
static esp_err_t on_usb_frame(void *buffer, uint16_t len, void *ctx)
{
    (void)ctx;
    if (s_netif == NULL || len == 0) {
        return ESP_OK;
    }
    void *copy = malloc(len);
    if (copy == NULL) {
        return ESP_ERR_NO_MEM;      /* drop: a lost frame beats a wedged stack */
    }
    memcpy(copy, buffer, len);
    esp_netif_receive(s_netif, copy, len, NULL);
    return ESP_OK;
}

static esp_err_t usb_post_attach(esp_netif_t *netif, void *args)
{
    (void)args;
    esp_netif_driver_ifconfig_t ifcfg = {
        .handle = (void *)1,        /* no driver object: the class is a singleton */
        .transmit = usb_transmit,
        .driver_free_rx_buffer = usb_free_rx,
    };
    ESP_RETURN_ON_ERROR(esp_netif_set_driver_config(netif, &ifcfg), TAG,
                        "cannot set the driver config");
    /* Nothing else brings this interface up — there is no link-detect on a USB
     * class device, so it is up from the moment it is attached. */
    esp_netif_action_start(netif, NULL, 0, NULL);
    return ESP_OK;
}

esp_netif_t *usb_net_netif(void)
{
    return s_netif;
}

esp_err_t usb_net_start(void)
{
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "cannot init esp_netif");

    /* No gateway: `gw` stays 0.0.0.0, and the DHCP server is told below to advertise
     * neither a router nor a DNS server. This is the feature, not an omission — a host
     * that accepts us as its gateway sends everything to a device with no uplink, and
     * macOS ranks a wired service above Wi-Fi, so it would do exactly that. The phone
     * keeps its own default route; we are reachable on our own subnet and nowhere else. */
    esp_netif_ip_info_t ip = { 0 };
    ESP_RETURN_ON_ERROR(esp_netif_str_to_ip4(USB_NET_ADDR, &ip.ip), TAG, "bad address");
    ESP_RETURN_ON_ERROR(esp_netif_str_to_ip4(USB_NET_MASK, &ip.netmask), TAG, "bad netmask");

    esp_netif_inherent_config_t base = ESP_NETIF_INHERENT_DEFAULT_ETH();
    base.if_key = "USBNCM";
    base.if_desc = "usb";
    base.ip_info = &ip;
    /* A DHCP server, not a client: this interface serves the host rather than
     * asking anyone for an address. AUTOUP so it comes up without a link event. */
    base.flags = (esp_netif_flags_t)(ESP_NETIF_DHCP_SERVER | ESP_NETIF_FLAG_AUTOUP);
    base.get_ip_event = 0;
    base.lost_ip_event = 0;

    /* Static, not stack: esp_netif keeps this pointer past esp_netif_new, and a
     * stack copy would be dangling by the time post_attach runs. Confirm against
     * the header in step 1 — if esp_netif copies it, the static costs nothing. */
    static esp_netif_driver_ifconfig_t driver = {
        .handle = (void *)1,
        .post_attach = usb_post_attach,
    };

    esp_netif_config_t cfg = {
        .base = &base,
        .driver = &driver,
        .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH,
    };

    s_netif = esp_netif_new(&cfg);
    ESP_RETURN_ON_FALSE(s_netif != NULL, ESP_FAIL, TAG, "cannot create the netif");

    const tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    ESP_RETURN_ON_ERROR(tinyusb_driver_install(&tusb_cfg), TAG,
                        "cannot install the TinyUSB driver");

    tinyusb_net_config_t net_cfg = {
        .on_recv_callback = on_usb_frame,
    };
    ESP_RETURN_ON_ERROR(esp_read_mac(net_cfg.mac_addr, ESP_MAC_WIFI_STA), TAG,
                        "cannot read the MAC");
    ESP_RETURN_ON_ERROR(tinyusb_net_init(&net_cfg), TAG, "cannot init the NCM class");

    /* The netif's own MAC must differ from the one the host sees on its side of the
     * wire, or both ends answer to the same address. Flip the locally-administered
     * bit for ours. */
    uint8_t our_mac[6];
    memcpy(our_mac, net_cfg.mac_addr, sizeof(our_mac));
    our_mac[0] |= 0x02;
    ESP_RETURN_ON_ERROR(esp_netif_set_mac(s_netif, our_mac), TAG, "cannot set the MAC");

    ESP_RETURN_ON_ERROR(esp_netif_attach(s_netif, &driver), TAG, "cannot attach");

    /* Suppress both DHCP adverts, before the server starts. Zero is neither
     * OFFER_ROUTER nor OFFER_DNS: the host gets an address and a netmask, and keeps
     * its own router and resolvers. Advertising DNS is as damaging as advertising a
     * route — queries into a black hole break a host that still has a correct default. */
    uint8_t offer = 0;
    ESP_RETURN_ON_ERROR(esp_netif_dhcps_option(s_netif, ESP_NETIF_OP_SET,
                                               ESP_NETIF_ROUTER_SOLICITATION_ADDRESS,
                                               &offer, sizeof(offer)), TAG, "cannot drop the router advert");
    ESP_RETURN_ON_ERROR(esp_netif_dhcps_option(s_netif, ESP_NETIF_OP_SET,
                                               ESP_NETIF_DOMAIN_NAME_SERVER,
                                               &offer, sizeof(offer)), TAG, "cannot drop the DNS advert");

    ESP_LOGI(TAG, "usb net up on %s", USB_NET_ADDR);
    return ESP_OK;
}
```

- [ ] **Step 4: Reduce `main.c` to the call**

```c
#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "usb_net.h"

static const char *TAG = "dongle";

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(usb_net_start());
    ESP_LOGI(TAG, "dongle up");
}
```

And add the new source in `firmware/s3/main/CMakeLists.txt`:

```cmake
idf_component_register(SRCS "main.c" "usb_net.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES nvs_flash esp_netif)
```

- [ ] **Step 5: Build, flash, and verify the Mac gets an address**

```bash
source tools/env-p4.sh
cd firmware/s3 && idf.py -p /dev/cu.<uart-bridge-port> flash monitor
```

Expected in the monitor: `usb net up on 192.168.7.1` then `dongle up`.

With the native USB port connected to the Mac:

```bash
ifconfig | grep -B2 -A4 "192.168.7"
ping -c 3 192.168.7.1
```

Expected: the interface holds `192.168.7.2` (given by the dongle's DHCP server) and
three replies from `192.168.7.1`.

Two failures worth telling apart. An interface with a `169.254.x.x` self-assigned
address means the DHCP server is not answering — check the `ESP_NETIF_DHCP_SERVER`
flag took effect. An interface with the right address but no ping replies means
frames go out and nothing comes back — check `usb_transmit`'s return value in the log.

- [ ] **Step 6: Commit**

```bash
git add firmware/s3
git commit -m "feat(s3): the dongle is an endpoint — its own address, and DHCP for the host"
```

---

### Task 4: `GET /status`, so the iPhone can be tested with nothing but Safari

**Files:**
- Create: `firmware/s3/main/status_api.h`
- Create: `firmware/s3/main/status_api.c`
- Modify: `firmware/s3/main/main.c`
- Modify: `firmware/s3/main/CMakeLists.txt`

**Interfaces:**
- Consumes: `usb_net_start()` from Task 3.
- Produces: `esp_err_t status_api_start(void)`, serving `GET /status` on port 80 of `192.168.7.1`. Plan 2 adds `POST /net` here, and adds an accessor for the server handle at the moment it has a caller — not before.

- [ ] **Step 1: Write `status_api.h`**

```c
#ifndef STATUS_API_H
#define STATUS_API_H

#include "esp_err.h"

/* Starts the HTTP server and registers GET /status.
 *
 * Bound to the USB interface only. The dongle's configuration must never be
 * reachable over the radio — from Plan 2 this endpoint carries a car's password. */
esp_err_t status_api_start(void);

#endif /* STATUS_API_H */
```

- [ ] **Step 2: Write `status_api.c`**

```c
#include <stdio.h>

#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"

#include "status_api.h"
#include "usb_net.h"

static const char *TAG = "status_api";

static httpd_handle_t s_server;

static esp_err_t status_get(httpd_req_t *req)
{
    const esp_app_desc_t *app = esp_app_get_description();

    char body[192];
    int n = snprintf(body, sizeof(body),
                     "{\"dev\":\"ajdongle\",\"fw\":\"%s\",\"idf\":\"%s\",\"usb\":\"up\"}",
                     app->version, app->idf_ver);
    if (n < 0 || n >= (int)sizeof(body)) {
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, n);
}

esp_err_t status_api_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    /* Plan 2 adds POST /net; leave room so that does not become a config change
     * disguised as a feature. */
    cfg.max_uri_handlers = 4;

    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &cfg), TAG, "cannot start the server");

    static const httpd_uri_t status_uri = {
        .uri = "/status",
        .method = HTTP_GET,
        .handler = status_get,
    };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &status_uri), TAG,
                        "cannot register GET /status");

    ESP_LOGI(TAG, "http://%s/status", USB_NET_ADDR);
    return ESP_OK;
}
```

- [ ] **Step 3: Call it from `main.c` and register the source**

In `main.c`, after `usb_net_start()`:

```c
    ESP_ERROR_CHECK(usb_net_start());
    ESP_ERROR_CHECK(status_api_start());
    ESP_LOGI(TAG, "dongle up");
```

with `#include "status_api.h"` alongside the existing includes, and in
`firmware/s3/main/CMakeLists.txt`:

```cmake
idf_component_register(SRCS "main.c" "usb_net.c" "status_api.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES nvs_flash esp_netif esp_http_server esp_app_format)
```

- [ ] **Step 4: Build, flash, verify from the Mac**

```bash
source tools/env-p4.sh
cd firmware/s3 && idf.py -p /dev/cu.<uart-bridge-port> flash monitor
curl -s http://192.168.7.1/status
```

Expected: `{"dev":"ajdongle","fw":"...","idf":"v6.0.2","usb":"up"}`

- [ ] **Step 5: Commit**

```bash
git add firmware/s3
git commit -m "feat(s3): GET /status, so the dongle can be checked from a browser"
```

---

### Task 5: The iPhone, and the answer

This is what the plan exists for. Everything before it was making the question askable.

**Files:**
- Modify: `firmware/s3/README.md`
- Modify: `docs/research/2026-08-21-usb-ethernet-dongle.md`

- [ ] **Step 1: Connect the dongle to the iPhone**

Native USB port → iPhone USB-C, with a C-to-C cable (or the USB-A adapter path from
Task 1 step 3 if this board has no CC resistors).

Keep the UART bridge port connected to the Mac with `idf.py monitor` running: the
logs are the only view into what the dongle thinks is happening, and this is exactly
the case the two-port board was chosen for.

- [ ] **Step 2: Look for the interface on the phone**

Settings → General → About, and Settings → Wi-Fi. An Ethernet interface appears as
its own row when iOS has bound a driver to the device.

- [ ] **Step 3: Open `http://192.168.7.1/status` in Safari**

Expected: the JSON from Task 4, rendered as text.

This single request proves the entire chain at once: iOS enumerated the device, bound
a CDC-NCM driver, accepted an address from the dongle's DHCP server, routed a TCP
connection over the wired interface, and got an answer back.

- [ ] **Step 4: Record the answer, whichever it is**

Fill the README's table with a fourth row, and add a section to the design note under
"Открытые вопросы, по убыванию важности" replacing question 1 with what was observed.

If it worked, say so plainly with the date, and note whether the phone kept its Wi-Fi
and cellular while the dongle was attached — that is the whole point of the idea and
it deserves its own observed line, not an assumption.

If it did not work, record **which step failed**, because the four failure points want
different answers:

| Failure | Likely cause | Where to go next |
|---|---|---|
| No power to the board at all | CC resistors (Task 1) | Solder them, or use the USB-A adapter path |
| Powered, no interface in Settings | iOS refuses class-compliant NCM | The idea is dead in this form; the design note's fallbacks (a Linux stick that can present other classes) become the only path |
| Interface present, self-assigned address | iOS ignores the DHCP offer | Try a static address on the iOS side to separate DHCP from NCM |
| Address fine, Safari times out | routing, not enumeration | The `.wiredEthernet` binding in Plan 4 is exactly the fix for this class of problem — note it and continue |

The third and fourth rows are recoverable; the second is not, and it is the one this
plan was built to detect early.

- [ ] **Step 5: Commit**

```bash
git add firmware/s3/README.md docs/research/2026-08-21-usb-ethernet-dongle.md
git commit -m "docs(s3): what iOS did with a class-compliant NCM device"
```

---

## After this plan

If Task 5 came back yes, **Plan 2** gives the dongle its configuration: `POST /net` to
set an AP, `GET /net` to read back what it is set to (never the password), NVS
persistence as one JSON string per domain, and the validation host-tested under
`firmware/s3/test/`. No radio, no car — a laptop and `curl` exercise all of it.

**Plan 3** then switches the radio on and makes the dongle a proxy, and it inherits
two questions this plan deliberately did not answer:

1. **The station and the USB interface cannot share the station MAC** — Task 2 borrows
   it, and flags in a comment that a station coming up later makes two interfaces claim
   one address.
2. **Port 80 is claimed twice.** The dongle serves its own API there, and the car serves
   its REST API on the same port. Proxying by port would force one of them to move; the
   design note's answer is to keep them on **separate addresses** instead, so the car
   keeps its whole port space — 80, the RT port, and whatever video wants later. Which
   address the car appears on, and whether iOS routes to it or the dongle aliases it
   locally, is Plan 3's first decision and must be settled before its Task 1.

If Task 5 came back no at the second row — powered, but no interface in Settings — stop
and re-read the design note's "Запасной ход остаётся открытым бесплатно". The seam was
chosen so the dongle's internals can be replaced without the app noticing, and that is
the option that gets exercised.
