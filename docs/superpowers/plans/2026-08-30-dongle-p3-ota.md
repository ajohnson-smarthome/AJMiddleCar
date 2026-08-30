# Dongle OTA Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The dongle accepts a firmware image over USB, boots it, and reverts it if that boot
fails — and one release carries the images for both devices under one version.

**Architecture:** `POST /ota` mirrors the car's handler minus the actuator layer the dongle does
not have. `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` switches on in the same branch, with
`app_main` marking the image valid only after the endpoint that could deliver a replacement is
answering — so rollback protects reachability, which is the only property that matters on a device
with no cable. `version.txt` returns to the repo root, both firmwares read it, and `release.sh`
attaches two binaries to one tag.

**Tech Stack:** ESP-IDF 6.0.2, `esp_ota_ops` (`app_update`), `esp_http_server`, CMake, bash, Python 3.

**Spec:** `docs/superpowers/specs/2026-08-30-dongle-api-design.md`

**Numbering:** this plan takes the P3 slot. The radio-and-NAT plan the spec argues for becomes P4,
the app-side plan P5. The order changed because OTA needs neither a car nor a radio to verify, and
because it makes every later bench session cheaper — once it lands, updating the dongle stops
needing a cable.

## Global Constraints

- **OTA must not exist in any commit that predates rollback.** The spec: "it is the one piece
  that must not ship before rollback does." Task 2 enables rollback; Task 3 adds the endpoint.
  That order is a requirement, not a preference — do not reorder them, and do not combine them
  into one commit that could be partially reverted.
- **The dongle knows nothing about the car.** No `firmware/s3` file may reference `firmware/p4`,
  its paths, its identity, or its credentials. This is why Task 4 moves `version.txt` to the root
  rather than having the dongle read the car's copy.
- **Generated files are never hand-edited.** `firmware/s3/main/dongle_contract.inc` and
  `app/AJMiddleCar/Generated/DongleAPI.swift` come from `contract/dongle-api.json` through
  `tools/gen_contract.py`. Change the schema and the emitter, then regenerate.
- **No real network credentials anywhere.** Not in tests, not in scripts, not in docs. Bench
  fixtures use neutral names.
- **ESP-IDF 6.0.2**, sourced with `tools/env-p4.sh` (target-agnostic; the target comes from
  `sdkconfig.defaults`).
- **`tools/test-all.sh` must be green before every commit.**

---

## File Structure

| File | Responsibility |
|---|---|
| `contract/dongle-api.json` | *modify* — gains the `/ota` endpoint and the `rollback` status field |
| `tools/gen_dongle.py` | *modify* — emits `DONGLE_PATH_OTA` and `DongleContract.otaPath` |
| `tools/test_gen_contract.py` | *modify* — asserts the two new names reach both artifacts |
| `firmware/s3/main/dongle_contract.inc` | *generated* — regenerate, never edit |
| `app/AJMiddleCar/Generated/DongleAPI.swift` | *generated* — regenerate, never edit |
| `firmware/s3/sdkconfig.defaults` | *modify* — rollback on, and the comment that said why it was off is rewritten to say why it is on |
| `firmware/s3/main/main.c` | *modify* — marks the running image valid, last thing in `app_main` |
| `firmware/s3/main/status_api.c` | *modify* — reads the rollback verdict once at boot, reports it |
| `firmware/s3/main/ota_api.{c,h}` | *create* — `POST /ota` |
| `firmware/s3/main/CMakeLists.txt` | *modify* — new source, `app_update` and `esp_partition` |
| `firmware/s3/verify-on-host.sh` | *modify* — the two refusals that are safe to automate |
| `firmware/s3/README.md` | *modify* — how to push an image, and what to expect |
| `version.txt` | *move* — from `firmware/p4/`, because the version belongs to the release, not to either firmware |
| `firmware/p4/CMakeLists.txt` | *modify* — reads the root copy, and fails loudly if it is missing |
| `firmware/s3/CMakeLists.txt` | *modify* — gains the same `PROJECT_VER` derivation |
| `tools/release.sh` | *modify* — one tag, two assets |

---

### Task 1: The contract learns `/ota` and `rollback`

The endpoint path and the new status key are vocabulary, so they come from the schema like every
other name the two sides must spell identically. `gen_dongle.py`'s status-field emitter already
loops over the schema, so `rollback` needs no emitter change; the endpoint emitters name each path
explicitly, so `/ota` needs one line in each.

**Files:**
- Modify: `contract/dongle-api.json`
- Modify: `tools/gen_dongle.py`
- Modify: `tools/test_gen_contract.py`
- Generated (regenerate, do not hand-edit): `firmware/s3/main/dongle_contract.inc`,
  `app/AJMiddleCar/Generated/DongleAPI.swift`

**Interfaces:**
- Produces: `DONGLE_PATH_OTA` (`"/ota"`) and `DONGLE_KEY_ROLLBACK` (`"rollback"`) in C;
  `DongleContract.otaPath` and `DongleStatusKey.rollback` in Swift. Tasks 2 and 3 use both.

- [ ] **Step 1: Add the endpoint and the status field to the schema**

In `contract/dongle-api.json`, the `endpoints` object gains `ota`, and `status_fields` gains
`rollback`. Keep `rollback` next to `usb` — both are facts about the device itself, ahead of the
nested `net` object, and the emitters preserve insertion order:

```json
  "endpoints": {
    "status": "/status",
    "net": "/net",
    "ota": "/ota"
  },
  "status_fields": {
    "device": "device",
    "fw": "fw",
    "idf": "idf",
    "usb": "usb",
    "rollback": "rollback",
    "net": "net",
    "net_ssid": "ssid",
    "net_state": "state",
    "net_rssi": "rssi"
  },
```

- [ ] **Step 2: Teach both emitters the new path**

In `tools/gen_dongle.py`, `emit_dongle_c` — after the `DONGLE_PATH_NET` line:

```python
        f'#define DONGLE_PATH_NET "{e["net"]}"',
        f'#define DONGLE_PATH_OTA "{e["ota"]}"',
```

and in `emit_dongle_swift`, after the `netPath` line:

```python
        f'    public static let netPath = "{e["net"]}"',
        f'    public static let otaPath = "{e["ota"]}"',
```

Nothing else changes: `status_fields` is emitted by a loop in both emitters, so `rollback`
arrives on its own as `DONGLE_KEY_ROLLBACK` and `DongleStatusKey.rollback`.

- [ ] **Step 3: Extend the generator tests**

`tools/test_gen_contract.py` already has a `TestDongleEmitters` class whose `setUp` puts the
module in `self.g` and the parsed schema in `self.s`. The new names belong in the four tests that
already assert this vocabulary, not in new ones — a `DONGLE_PATH_OTA` test separate from
`test_c_header_carries_the_paths` would be a second place to look for the same fact.

Add one line to each of these four existing methods:

```python
    def test_c_header_carries_the_paths(self):
        out = self.g.emit_dongle_c(self.s)
        self.assertIn('#define DONGLE_PATH_STATUS "/status"', out)
        self.assertIn('#define DONGLE_PATH_NET "/net"', out)
        self.assertIn('#define DONGLE_PATH_OTA "/ota"', out)
```

```python
    def test_c_header_carries_the_status_and_net_keys(self):
        ...
        self.assertIn('#define DONGLE_KEY_ROLLBACK "rollback"', out)
```

```python
    def test_swift_exposes_the_same_vocabulary(self):
        ...
        self.assertIn('public static let otaPath = "/ota"', out)
```

```python
    def test_swift_exposes_the_status_keys(self):
        ...
        self.assertIn('public static let rollback = "rollback"', out)
```

Put each new `assertIn` next to its neighbours — the OTA path after `netPath`, the rollback key
after `usb` — and leave the surrounding comments alone.

- [ ] **Step 4: Run them and watch them fail**

Run: `python3 tools/test_gen_contract.py`
Expected: the two new tests fail — the emitters have the lines but the committed artifacts do not
yet, and `check_contract.sh` has not run. If they pass immediately, Step 2 was already applied;
confirm before continuing.

- [ ] **Step 5: Regenerate and verify**

```bash
python3 tools/gen_contract.py
python3 tools/test_gen_contract.py
bash tools/check_contract.sh
```

Expected: all tests pass; the drift check is silent. `git diff` shows `DONGLE_PATH_OTA` and
`DONGLE_KEY_ROLLBACK` in `firmware/s3/main/dongle_contract.inc`, and `otaPath` plus
`DongleStatusKey.rollback` in `app/AJMiddleCar/Generated/DongleAPI.swift`.

- [ ] **Step 6: Full suite, then commit**

```bash
tools/test-all.sh
git add contract/dongle-api.json tools/gen_dongle.py tools/test_gen_contract.py \
        firmware/s3/main/dongle_contract.inc app/AJMiddleCar/Generated/DongleAPI.swift
git commit -m "feat(contract): the dongle's vocabulary gains /ota and rollback"
```

---

### Task 2: Rollback, and the honest report of it

Rollback lands before the endpoint that needs it, so no commit in this branch's history contains
OTA without the net beneath it.

Enabling rollback alone changes behaviour for images written through `esp_ota_set_boot_partition`:
the bootloader marks them `PENDING_VERIFY`, and reverts them on the next boot unless the running
image cancels it. Nothing writes such an image yet, so this task is inert on a cable-flashed
dongle — which is exactly why it is safe to land first.

**Where the mark goes, and why at the end.** The car marks early, before its API registrations,
because a mismatched radio can stall `status_api_start` for five seconds and a reset inside that
window used to revert a good update. The dongle has no such long pole: nothing between
`usb_net_start()` and the end of `app_main` blocks for more than milliseconds. So the mark goes
last, after the HTTP server is up and both endpoint sets are registered — because the property
worth protecting on a device with no cable is *"the app can still reach it to push a replacement"*.
An image that comes up but cannot serve `/ota` is precisely the image that must not be kept.

One consequence to state plainly: everything before the mark is a rollback trigger. The existing
`ESP_ERROR_CHECK`s in `app_main` panic-reboot on failure, and a panic before the mark is what
makes the bootloader revert. That is correct and deliberate.

**Files:**
- Modify: `firmware/s3/sdkconfig.defaults`
- Modify: `firmware/s3/main/main.c`
- Modify: `firmware/s3/main/status_api.c`
- Modify: `firmware/s3/main/CMakeLists.txt`

**Interfaces:**
- Consumes: `DONGLE_KEY_ROLLBACK` from Task 1.
- Produces: `app_main` guarantees the running image is out of `PENDING_VERIFY` before it returns.
  Task 3's `esp_ota_begin` depends on that — see the note in its handler.

- [ ] **Step 1: Turn rollback on**

In `firmware/s3/sdkconfig.defaults`, replace the paragraph that ends
`It arrives with the OTA code that needs it.` The setting and its reason both change, so replace
the whole comment rather than appending to it:

```
# 16 MB flash with a custom table: two 4 MB OTA slots, plus a reserved data partition.
# The slots predate the OTA code by two plans — see partitions.csv for why the table is the half
# that gets decided first.
#
# Rollback is on. An image written through POST /ota boots once as PENDING_VERIFY and is reverted
# by the bootloader unless it cancels that itself; main.c cancels it at the end of app_main, after
# the HTTP server that could deliver a replacement is answering. The property this protects is
# reachability — a dongle lives in a pocket, and an image that boots but cannot serve /ota is one
# that needs a cable and a laptop to undo.
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y
```

- [ ] **Step 2: Declare the components the new calls need**

In `firmware/s3/main/CMakeLists.txt`, add `app_update` (for `esp_ota_ops.h`) and `esp_partition`
(for `esp_partition.h`) to `PRIV_REQUIRES`. Declare only these two — an earlier commit on this
firmware trimmed over-declared requirements, and that trim should stay meaningful:

```cmake
idf_component_register(SRCS "main.c" "usb_net.c" "status_api.c" "api_util.c" "net_api.c" "net_cfg.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES nvs_flash esp_netif esp_event esp_http_server esp_app_format
                                     app_update esp_partition cjson)
```

- [ ] **Step 3: Mark the image valid at the end of `app_main`**

In `firmware/s3/main/main.c`, add the include:

```c
#include "esp_ota_ops.h"
#include "esp_partition.h"
```

and replace the final `ESP_LOGI(TAG, "dongle up");` with:

```c
    /* Rollback is waived here and nowhere earlier. Everything above is a rollback trigger:
       the ESP_ERROR_CHECKs panic-reboot on failure, and a panic while the image is still
       PENDING_VERIFY is what makes the bootloader put the previous one back. By this line the
       USB netif is attached and the server is answering on DONGLE_PORT, which is the whole
       property worth protecting — an image that boots but cannot serve /ota is an image that
       needs a cable to undo, and this device lives in a pocket.

       The guard matters: mark_app_valid on an image that is NOT pending is harmless but noisy,
       and reading the state first keeps a normal cable-flashed boot silent. */
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    if (running != NULL &&
        esp_ota_get_state_partition(running, &ota_state) == ESP_OK &&
        ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "first boot of a new image — cancelling rollback");
        esp_ota_mark_app_valid_cancel_rollback();
    }
    ESP_LOGI(TAG, "dongle up");
```

- [ ] **Step 4: Report a rollback that already happened**

A silent revert is worse on the dongle than on the car. The app compares `/status`'s `fw` against
the release and offers an update when it is behind; if the bootloader quietly put the old image
back, the app sees the old version, offers the same update again, and loops forever with no
explanation. This field is what breaks that loop.

In `firmware/s3/main/status_api.c`, add the includes:

```c
#include "esp_ota_ops.h"
#include "esp_partition.h"
```

and, above `status_get`:

```c
/* Did the bootloader revert the previous OTA? The other slot is left ESP_OTA_IMG_ABORTED exactly
 * when an update failed its first boot — the one signal a client has that the image it pushed did
 * not survive. Read once at start: the answer cannot change without a reboot. The car's
 * status_api.c carries the twin of this; the duplication is the price of the two firmwares not
 * referencing each other. */
static bool s_rollback = false;

static void read_rollback_state(void)
{
    const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
    esp_ota_img_states_t st;
    s_rollback = other != NULL &&
                 esp_ota_get_state_partition(other, &st) == ESP_OK &&
                 st == ESP_OTA_IMG_ABORTED;
    if (s_rollback) ESP_LOGW(TAG, "the previous OTA was rolled back by the bootloader");
}
```

Add `#include <stdbool.h>` at the top of the include block — `esp_http_server.h` happens to
reach it today, but a file that declares a `bool` should say where the type comes from.

- [ ] **Step 5: Call it, and put the field in the body**

In `status_api_start`, call `read_rollback_state();` as the first statement — before `httpd_start`,
so no request can observe the default.

In `status_get`, add the key after `usb` and grow the buffer:

```c
    /* 320, not 256. Worst case with the rollback field: 103 bytes of literal template,
     * + 31 (esp_app_desc_t.version is char[32]) + 31 (idf_ver, likewise) + 5 ("false")
     * + 64 (a 32-byte SSID whose every byte escapes to two) + NUL = 235. The margin is
     * deliberate: adding one field should not also be a buffer calculation. */
    char body[320];
    int n = snprintf(body, sizeof(body),
                     "{\"" DONGLE_KEY_DEVICE "\":\"" DONGLE_DEVICE "\","
                     "\"" DONGLE_KEY_FW "\":\"%s\","
                     "\"" DONGLE_KEY_IDF "\":\"%s\","
                     "\"" DONGLE_KEY_USB "\":\"" DONGLE_USB_STATE_UP "\","
                     "\"" DONGLE_KEY_ROLLBACK "\":%s,"
                     "\"" DONGLE_KEY_NET "\":{"
                     "\"" DONGLE_KEY_NET_SSID "\":\"%s\","
                     "\"" DONGLE_KEY_NET_STATE "\":\"" DONGLE_STATE_IDLE "\","
                     "\"" DONGLE_KEY_NET_RSSI "\":0}}",
                     app->version, app->idf_ver, s_rollback ? "true" : "false", ssid_esc);
```

Leave the existing truncation guard below it exactly as it is.

- [ ] **Step 6: Build it**

```bash
source tools/env-p4.sh && (cd firmware/s3 && idf.py build)
```

Expected: a clean build. On a machine that has never built for this target it fails on a missing
Xtensa toolchain — `~/esp/esp-idf-v6.0.2/install.sh esp32s3` once, as `firmware/s3/README.md` says.

Then confirm rollback actually reached the config, rather than trusting the defaults file:

```bash
grep BOOTLOADER_APP_ROLLBACK_ENABLE firmware/s3/sdkconfig
```

Expected: `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`. A `# ... is not set` line means the stale
`sdkconfig` in the tree predates the change — delete `firmware/s3/sdkconfig` and rebuild.

- [ ] **Step 7: Full suite, then commit**

```bash
tools/test-all.sh
git add firmware/s3/sdkconfig.defaults firmware/s3/main/main.c \
        firmware/s3/main/status_api.c firmware/s3/main/CMakeLists.txt
git commit -m "feat(s3): rollback on, and a boot that earns its keep before waiving it"
```

---

### Task 3: `POST /ota`

The car's handler, minus the actuator layer. Every check that guards the flash stays; the
`car_stop`/`link_release_must` pairs around each failure path go, because the dongle has nothing
that moves.

**Files:**
- Create: `firmware/s3/main/ota_api.c`, `firmware/s3/main/ota_api.h`
- Modify: `firmware/s3/main/main.c`, `firmware/s3/main/CMakeLists.txt`
- Modify: `firmware/s3/verify-on-host.sh`, `firmware/s3/README.md`

**Interfaces:**
- Consumes: `DONGLE_PATH_OTA` (Task 1); `api_reply_error` / `api_reply_ok` from `api_util.h`;
  `status_api_server()` for the running `httpd_handle_t`; Task 2's guarantee that the running
  image is no longer `PENDING_VERIFY` by the time `app_main` returns.
- Produces: `esp_err_t ota_api_register(httpd_handle_t server);`

- [ ] **Step 1: The header**

Create `firmware/s3/main/ota_api.h`:

```c
#ifndef OTA_API_H
#define OTA_API_H

#include "esp_err.h"
#include "esp_http_server.h"

/* Registers POST /ota on an already-running server — streams an app image into the inactive
 * slot, validates it, and reboots into it. Takes the handle rather than starting a server of
 * its own, the way net_api_register does: status_api owns the one server this firmware has.
 *
 * Same warning as status_api.h: this server is not bound to the USB interface. When Plan 4
 * links a radio, /ota becomes reachable from the car's network alongside POST /net — and an
 * unauthenticated firmware-write endpoint is a worse thing to expose than a password. Whoever
 * brings up the station brings the peer check first. */
esp_err_t ota_api_register(httpd_handle_t server);

#endif /* OTA_API_H */
```

- [ ] **Step 2: The handler**

Create `firmware/s3/main/ota_api.c`:

```c
#include "ota_api.h"

#include <limits.h>
#include <stdint.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "api_util.h"
#include "dongle_contract.inc"

static const char *TAG = "ota_api";

/* A deliberate twin of firmware/p4/main/ota_api.c, not a shared file — the two firmwares do not
 * reference each other. What is missing here is the car's actuator arbitration: the car seizes
 * the motors for the length of the flash and releases them on every failure path, because a
 * refused upload must not leave a car undriveable. The dongle has nothing that moves, so that
 * layer is absent rather than stubbed out. Every check that guards the flash itself is kept. */
static esp_err_t ota_post(httpd_req_t *req)
{
    if (req->content_len < 4096) {  /* reject obviously-bogus uploads before erasing a slot */
        return api_reply_error(req, "400 Bad Request", "", "image too small");
    }
    if (req->content_len > INT_MAX) {  /* guard the (int) cast below: a huge len wraps negative */
        return api_reply_error(req, "400 Bad Request", "", "image too large");
    }
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (part == NULL) {
        api_reply_error(req, "500 Internal Server Error", "", "no ota partition");
        return ESP_FAIL;
    }
    if ((uint32_t)req->content_len > part->size) {
        return api_reply_error(req, "400 Bad Request", "", "image too large");
    }

    esp_ota_handle_t handle = 0;
    /* The exact length is known from Content-Length: erasing only what the image needs, instead
       of OTA_SIZE_UNKNOWN's full 4 MB, saves seconds of erase and flash wear per update — and a
       too-large image fails above rather than after the erase. */
    esp_err_t berr = esp_ota_begin(part, req->content_len, &handle);
    if (berr != ESP_OK) {
        if (berr == ESP_ERR_OTA_ROLLBACK_INVALID_STATE) {
            /* The running image is still PENDING_VERIFY, so IDF refuses to start another update.
               app_main cancels rollback before it returns, so reaching this needs a request that
               beat the last instruction of boot — which USB enumeration alone makes implausible.
               It gets its own message anyway: "ota begin failed" would send someone hunting the
               flash for a fault that is really a race. */
            ESP_LOGE(TAG, "refusing: this image has not finished verifying its own boot");
            return api_reply_error(req, "409 Conflict", "", "image still pending verify");
        }
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(berr));
        api_reply_error(req, "500 Internal Server Error", "", "ota begin failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "OTA -> %s, %d bytes", part->label, (int)req->content_len);

    char buf[1024];
    int remaining = (int)req->content_len;
    int timeouts = 0;  /* bound stalls: a silent client must not wedge the single httpd task */
    while (remaining > 0) {
        int chunk = remaining < (int)sizeof(buf) ? remaining : (int)sizeof(buf);
        int r = httpd_req_recv(req, buf, chunk);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts <= 6) continue;  /* ~6x5s, then abort */
            esp_ota_abort(handle);
            api_reply_error(req, "400 Bad Request", "", "recv error");
            return ESP_FAIL;
        }
        timeouts = 0;  /* progress resets the stall budget */
        /* esp_ota_write rejects a first byte that is not 0xE9 (ESP_IMAGE_HEADER_MAGIC) with
           ESP_ERR_OTA_VALIDATE_FAILED — the spec's "reject anything whose first byte is not the
           ESP image magic" is satisfied by IDF, not by a check of ours. Do not add a second one:
           a hand-rolled magic test would be a copy of app_update's that could drift from it. */
        if (esp_ota_write(handle, buf, r) != ESP_OK) {
            esp_ota_abort(handle);
            api_reply_error(req, "400 Bad Request", "", "image invalid");
            return ESP_FAIL;
        }
        remaining -= r;
    }

    if (esp_ota_end(handle) != ESP_OK) {
        api_reply_error(req, "400 Bad Request", "", "image invalid");
        return ESP_FAIL;
    }
    esp_err_t serr = esp_ota_set_boot_partition(part);
    if (serr != ESP_OK) {
        ESP_LOGE(TAG, "set_boot_partition failed: %s (image written+valid but not booted)",
                 esp_err_to_name(serr));
        api_reply_error(req, "500 Internal Server Error", "", "set boot failed");
        return ESP_FAIL;
    }
    /* Reboot whether or not the "ok" reaches the client — the image is already committed. The
       client will see the USB interface drop and come back; that is the update completing, not
       a failure. */
    if (api_reply_ok(req) != ESP_OK) ESP_LOGW(TAG, "resp send failed, rebooting anyway");
    ESP_LOGI(TAG, "OTA done - rebooting");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

esp_err_t ota_api_register(httpd_handle_t server)
{
    if (server == NULL) {
        ESP_LOGE(TAG, "no server to register on");
        return ESP_ERR_INVALID_ARG;
    }
    static const httpd_uri_t ota_uri = {
        .uri = DONGLE_PATH_OTA,
        .method = HTTP_POST,
        .handler = ota_post,
    };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &ota_uri), TAG,
                        "cannot register POST /ota");
    return ESP_OK;
}
```

- [ ] **Step 3: Register it, before the mark-valid**

In `firmware/s3/main/main.c`, add `#include "ota_api.h"` and register `/ota` immediately after
`net_api_register`, so the rollback waiver added in Task 2 stays the last thing `app_main` does:

```c
    ESP_ERROR_CHECK(status_api_start());
    ESP_ERROR_CHECK(net_api_register(status_api_server()));
    ESP_ERROR_CHECK(ota_api_register(status_api_server()));
```

`status_api.c`'s `cfg.max_uri_handlers = 6` already covers this: `/status`, `GET /net`,
`POST /net`, `POST /ota` is four. Leave it — its comment already says it is sized for later
additions.

- [ ] **Step 4: Add the source**

In `firmware/s3/main/CMakeLists.txt`, add `"ota_api.c"` to `SRCS`. `PRIV_REQUIRES` already gained
`app_update` and `esp_partition` in Task 2.

- [ ] **Step 5: Build**

```bash
source tools/env-p4.sh && (cd firmware/s3 && idf.py build)
```

Expected: a clean build, and the size summary shows the image well under the 4 MB slot.

- [ ] **Step 6: Automate the two refusals that are safe to run**

In `firmware/s3/verify-on-host.sh`, inside the `DONGLE ATTACHED` block, after the short-password
check and before the `=== 2. THE REGRESSION ===` header:

```bash
      echo
      echo "POST /ota with a 2 KB body (must be refused as too small — nothing is erased):"
      head -c 2048 /dev/urandom > /tmp/ota-tiny.bin
      curl -s --max-time 10 -X POST --data-binary @/tmp/ota-tiny.bin \
           -H 'Content-Type: application/octet-stream' http://192.168.7.1:8080/ota \
           -w "\n  [HTTP %{http_code}]\n" 2>&1 | sed 's/^/  /'
      # This one DOES erase the inactive slot before IDF's magic check rejects the first write.
      # That is harmless here and worth knowing: after a successful update the inactive slot holds
      # the previous image, but rollback was already waived at boot, so nothing depends on it.
      echo "POST /ota with 8 KB of noise (must be refused — the first byte is not 0xE9):"
      head -c 8192 /dev/urandom > /tmp/ota-noise.bin
      curl -s --max-time 15 -X POST --data-binary @/tmp/ota-noise.bin \
           -H 'Content-Type: application/octet-stream' http://192.168.7.1:8080/ota \
           -w "\n  [HTTP %{http_code}]\n" 2>&1 | sed 's/^/  /'
      rm -f /tmp/ota-tiny.bin /tmp/ota-noise.bin
      echo "the dongle is still answering after two refused uploads:"
      curl -s --max-time 5 -w "\n  [HTTP %{http_code}]\n" http://192.168.7.1:8080/status 2>&1 | sed 's/^/  /'
```

Both bodies go through a file rather than a pipe on purpose: `curl` must send a
`Content-Length`, and the handler's first check reads `req->content_len`.

The real image push is NOT in this script — it reboots the device mid-run and would truncate
everything after it. It goes in the README as a manual step.

- [ ] **Step 7: Document the manual half**

In `firmware/s3/README.md`, after the `## Build` section, add:

```markdown
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
```

- [ ] **Step 8: Full suite, then commit**

```bash
tools/test-all.sh
git add firmware/s3/main/ota_api.c firmware/s3/main/ota_api.h firmware/s3/main/main.c \
        firmware/s3/main/CMakeLists.txt firmware/s3/verify-on-host.sh firmware/s3/README.md
git commit -m "feat(s3): the dongle takes its own firmware over the wire"
```

---

### Task 4: One release, two images

`release.sh` today reads `firmware/p4/version.txt` and ships one binary. The spec wants one tag
carrying both images under one version, so that "the hardware is behind this app" is a single
event rather than two policies that can disagree.

**Where the version lives.** It moves to the repo root. The dongle must not read
`firmware/p4/version.txt` — that would be the dongle knowing about the car, which is the one thing
this branch has been careful to prevent. A second `firmware/s3/version.txt` that must match the
first is a drift source, and the contract work exists precisely to remove those. The version
belongs to the release, and the release is a repo-level thing.

It lived at the root before the P4 migration moved it, for a mechanical reason: CMake read it as
`${CMAKE_CURRENT_LIST_DIR}/version.txt`, and the migration spec noted that left behind it "would
not fail loudly". Step 2 makes that failure loud, which is what makes the move safe this time.

**Files:**
- Move: `firmware/p4/version.txt` → `version.txt`
- Modify: `firmware/p4/CMakeLists.txt`, `firmware/s3/CMakeLists.txt`, `tools/release.sh`

**Interfaces:**
- Produces: both firmwares embed the same `PROJECT_VER` (`v<semver>+<git commit count>`), and a
  release carries `ajmiddlecar.bin` and `ajdongle.bin` under one tag.

- [ ] **Step 1: Move the file**

```bash
git mv firmware/p4/version.txt version.txt
```

- [ ] **Step 2: Point the car's CMake at the root, and make a missing file loud**

In `firmware/p4/CMakeLists.txt`, replace lines 3-12 (the comment block through the
`CMAKE_CONFIGURE_DEPENDS` property) with:

```cmake
# Firmware version = v<semver from the repo-root version.txt>+<git commit count>.
# Set PROJECT_VER BEFORE project() so esp_app_desc / esp_app_get_description()->version uses it
# (takes precedence over IDF's version.txt auto-detection and git-describe fallback).
#
# The file sits at the repo root, not here, because the version identifies a RELEASE and a release
# carries two images — this one and firmware/s3's. firmware/s3/CMakeLists.txt reads the same file
# the same way. The EXISTS check is not ceremony: the 2026-08-19 migration noted that a
# mispointed path "would not fail loudly", producing an empty semver and a car reporting a
# garbage version. It fails loudly now.
set(VERSION_FILE "${CMAKE_CURRENT_LIST_DIR}/../../version.txt")
if(NOT EXISTS "${VERSION_FILE}")
    message(FATAL_ERROR "version.txt not found at ${VERSION_FILE}")
endif()
file(STRINGS "${VERSION_FILE}" SEMVER LIMIT_COUNT 1)
# Re-run configure when version.txt changes. The commit COUNT still refreshes only on
# configure (a git state cannot be a configure dependency), so an incremental bench build
# after new commits reports the count of its last configure — the release path fullcleans,
# which is what makes release binaries exact. Recorded by the 2026-08-23 audit.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${VERSION_FILE}")
```

Leave the rest of the file — the `string(STRIP ...)`, the `execute_process` commit count, the
`set(PROJECT_VER ...)` and `project(ajmiddlecar)` — exactly as it is.

- [ ] **Step 3: Give the dongle the same derivation**

Replace `firmware/s3/CMakeLists.txt` entirely:

```cmake
cmake_minimum_required(VERSION 3.16)

# Firmware version = v<semver from the repo-root version.txt>+<git commit count> — the same
# string the car embeds, from the same file, because one release ships both images and the app
# compares one version against one tag. Set before project() so esp_app_get_description()->version
# carries it, which is what GET /status reports as `fw`.
set(VERSION_FILE "${CMAKE_CURRENT_LIST_DIR}/../../version.txt")
if(NOT EXISTS "${VERSION_FILE}")
    message(FATAL_ERROR "version.txt not found at ${VERSION_FILE}")
endif()
file(STRINGS "${VERSION_FILE}" SEMVER LIMIT_COUNT 1)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${VERSION_FILE}")
string(STRIP "${SEMVER}" SEMVER)
execute_process(
    COMMAND git rev-list --count HEAD
    WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}"
    OUTPUT_VARIABLE BUILD_NUM
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(NOT BUILD_NUM)
    set(BUILD_NUM "0")
endif()
set(PROJECT_VER "v${SEMVER}+${BUILD_NUM}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ajdongle)
```

- [ ] **Step 4: Two assets on one tag**

In `tools/release.sh`:

Replace the version block (the `grep -c` check through the `BIN=` line) with:

```bash
# version.txt must be exactly one line: CMake reads only the first, this script strips
# whitespace — a second line would let the tag and the embedded version disagree. It lives at
# the repo root because both firmwares read it: one release, one version, two images.
if [ "$(grep -c '' version.txt)" != 1 ]; then
    echo "ERROR: version.txt must be exactly one line"; exit 1
fi
SEMVER=$(tr -d '[:space:]' < version.txt)
[[ "$SEMVER" =~ ^[0-9]+\.[0-9]+(\.[0-9]+)?$ ]] || { echo "ERROR: version.txt must contain a bare semver (got: $SEMVER)"; exit 1; }
BUILD_NUM=$(git rev-list --count HEAD)
VER="v${SEMVER}+${BUILD_NUM}"
TITLE="v${SEMVER} (build ${BUILD_NUM})"
BIN_CAR="firmware/p4/build/ajmiddlecar.bin"
BIN_DONGLE="firmware/s3/build/ajdongle.bin"
NOTES="${NOTES_ARG:-Release ${VER}}"
```

In the `--dry-run` block, replace the single `asset` line and extend the `would run` line:

```bash
    echo "[dry-run] assets  : $BIN_CAR"
    echo "[dry-run]         : $BIN_DONGLE"
```
```bash
    echo "[dry-run] would run: test-all && rm sdkconfig && idf.py fullclean && idf.py build (p4, s3) && gh release create '$VER' '$BIN_CAR' '$BIN_DONGLE' --target <HEAD> ..."
```

Replace the build block (from `# A stray bench sdkconfig` through the `[ -f "$BIN" ]` guard) with:

```bash
# A stray bench sdkconfig must not configure a release: regenerate purely from defaults. This
# matters more for the dongle than for the car — bench work on the S3 has run with local
# overrides before, and a release built from one would ship them.
rm -f firmware/p4/sdkconfig firmware/p4/sdkconfig.old
rm -f firmware/s3/sdkconfig firmware/s3/sdkconfig.old
(cd firmware/p4 && idf.py fullclean >/dev/null && idf.py build)
[ -f "$BIN_CAR" ] || { echo "ERROR: $BIN_CAR not built"; exit 1; }
# The dongle is an Xtensa target; the car and its radio are both RISC-V, so an ESP-IDF installed
# for the car alone has no compiler for it. Say so rather than letting a toolchain error look
# like a firmware problem.
if ! (cd firmware/s3 && idf.py fullclean >/dev/null && idf.py build); then
    echo "ERROR: the dongle build failed. If this is a fresh ESP-IDF install, it has no Xtensa"
    echo "       toolchain yet: ~/esp/esp-idf-v6.0.2/install.sh esp32s3"; exit 1
fi
[ -f "$BIN_DONGLE" ] || { echo "ERROR: $BIN_DONGLE not built"; exit 1; }
```

And the release line:

```bash
gh release create "$VER" "$BIN_CAR" "$BIN_DONGLE" --target "$LOCAL_HEAD" --title "$TITLE" --notes "$NOTES"
```

- [ ] **Step 5: Prove it with a dry run**

```bash
tools/release.sh --dry-run
```

Expected: version, tag, title, both asset paths, the radio pin, notes — and exit 0 with no
network call and no build.

Then prove the validation still bites, restoring the file afterwards:

```bash
cp version.txt /tmp/v-good.txt
printf '1.0\nextra\n' > version.txt
{ tools/release.sh --dry-run; echo "exit=$?"; }
cp /tmp/v-good.txt version.txt
```

Expected: `ERROR: version.txt must be exactly one line`, `exit=1`, and `git status` clean
afterwards.

- [ ] **Step 6: Prove both builds embed the same version**

```bash
source tools/env-p4.sh
(cd firmware/p4 && idf.py build >/dev/null) && (cd firmware/s3 && idf.py build >/dev/null)
grep -ao 'v1\.0+[0-9]*' firmware/p4/build/ajmiddlecar.bin | head -1
grep -ao 'v1\.0+[0-9]*' firmware/s3/build/ajdongle.bin | head -1
```

Expected: the same string from both, and it matches `v1.0+$(git rev-list --count HEAD)`. This is
the whole point of the task — if the two differ, the app cannot use one version to gate both.

Note both builds must be freshly configured for the count to be current; if either was configured
before recent commits, `idf.py fullclean` it and rebuild.

- [ ] **Step 7: Full suite, then commit**

```bash
tools/test-all.sh
git add version.txt firmware/p4/CMakeLists.txt firmware/s3/CMakeLists.txt tools/release.sh
git commit -m "feat(release): one tag, one version, two images"
```

---

## What this plan does not do

- **The app side.** `UpdateClient.swift` gains a second asset name and a second cache path, and
  the update screen runs the dongle before the car. That is the app plan (P5) — it needs a
  simulator and it consumes what Task 1 generated. Nothing here touches `app/` except the
  generated `DongleAPI.swift`.
- **Bind the API to USB.** `/ota` joins `/net` on a server that listens on `INADDR_ANY`, and both
  headers now carry the warning. The guard belongs to the plan that brings up the radio (P4),
  because until then no second interface exists to reach it from. An unauthenticated write-firmware
  endpoint makes that guard more urgent, not less.
- **Signed images.** Secure boot and signature verification are not in the spec and not here. The
  device is reachable only over a USB cable the user is holding.

## Bench verification, when hardware is available

This plan's firmware is not host-testable — `ota_api.c` is entirely ESP-IDF calls, and there is no
pure module to extract from it. Task 1 and Task 4 are covered by the suite; Tasks 2 and 3 are
covered by the bench, and honestly so. In order:

1. Cable-flash the built image. `/status` answers with the new `fw`, and `rollback:false`.
2. `firmware/s3/verify-on-host.sh out.txt` — the Plan 1 and 2 checks still pass, and both OTA
   refusals return 400 with the dongle still answering afterwards.
3. Push a real image with the README's `curl`. Expect `{"ok":true}`, an interface drop, and
   `/status` back within seconds carrying the same `fw` and `rollback:false`.
4. The safety net, deliberately: build an image with a `return;` at the top of `app_main`, push
   it, and confirm the dongle comes back on the PREVIOUS image with `rollback:true`. This is the
   only test that proves rollback works, and it is the reason the whole feature is safe to have.
   Do not skip it, and do not commit that image.
