# Dongle — Plan 2: the network it is told to join

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The dongle holds a network configuration — an SSID and a password it was told at runtime — serves it over `GET`/`POST /net`, persists it across reboots, and reports it in `/status`.

**Architecture:** Validation and rendering live in a pure module with no ESP-IDF dependency, host-tested with plain `cc` exactly as the car's pure modules are. cJSON extraction, NVS and the HTTP handlers sit on the ESP side around it. No radio: this plan makes the dongle configurable, and Plan 3 makes it act on the configuration.

**Tech Stack:** C11, ESP-IDF 6.0.2, `esp_http_server`, `nvs_flash`, cJSON. Host tests with `cc` under `-Wall -Wextra -Werror -std=c11`.

**Spec:** `docs/superpowers/specs/2026-08-30-dongle-api-design.md` — read it first. It carries the reasoning for every choice below and is not repeated here.

## Global Constraints

- **The dongle knows nothing about any car.** No SSID, no password, no `device_id`, no protocol compiled in. The SSID it stores arrives at runtime and is an opaque string to it. Anything car-shaped appearing in `firmware/s3/` is a bug in this plan, not a feature.
- **`firmware/s3/` does not reference `app/` or `firmware/p4/`,** and neither references it. Where this plan duplicates a small helper the car also has, that duplication is the price of the rule and is deliberate.
- The dongle's own API moves to port **8080**. Port 80 is being reserved for the car in Plan 3.
- The identity key in `/status` is **`device`**, matching the car's `device_field`.
- Configuration persists in NVS as **one JSON string per domain**, with a dirty check so an unchanged POST does not rewrite flash.
- Conventions borrowed from the car, verbatim: `Content-Type: application/json`; success is `{"ok":true}`; rejection is `4xx` with `{"error":"…","field":"…"}`, `field` empty when the body as a whole is at fault; values are **rejected, never clamped**.
- Validation bounds, which are WPA2's rather than ours: SSID **1–32 bytes**; password **empty, or 8–63 bytes**.
- `tools/test-all.sh` must stay green, and from Task 2 onward it also runs this firmware's host tests.

## What this plan does not do

No radio. `esp_wifi` is not linked, no station is created, and nothing tries to join anything. `/status` therefore reports `net.state` as `idle` and never anything else — honestly, because there is nothing that could put it in another state.

That is not a stub. The shape of the surface is final: Plan 3 gives `state` its other values without changing a field name or a caller. The app can be written against this.

## Why the radio is not in this plan

The configuration surface is fully exercisable with a laptop and `curl` — set a network, read it back, restart the dongle, read it again. The radio needs a car, a bench and a place to drive. Keeping them apart means the half that can be tested anywhere is finished and reviewed before the half that cannot.

It also matters for a specific hazard the spec names: this plan adds an endpoint that carries a Wi-Fi password to a server that binds `INADDR_ANY`, and what keeps it USB-only today is that no other interface exists. Plan 3 is where a radio appears, and Plan 3 is therefore where the guard must appear. Shipping them together would make it easy to ship the password endpoint and forget the guard.

## File Structure

| File | Responsibility |
|---|---|
| `firmware/s3/main/net_cfg.{c,h}` | **Pure.** Validates an SSID and password, renders both JSON shapes, and maps each rejection to its field and message. No ESP-IDF headers |
| `firmware/s3/test/test_net_cfg.c` | Host test for the above |
| `firmware/s3/test/Makefile` | Host-test harness, modelled on `firmware/p4/test/Makefile` |
| `firmware/s3/main/api_util.{c,h}` | The shared reply shapes and a whole-body reader. A deliberate small twin of the car's, because the two firmwares may not share code |
| `firmware/s3/main/net_api.{c,h}` | `GET`/`POST /net`: cJSON extraction, NVS persistence, the handlers |
| `firmware/s3/main/status_api.c` | Moves to port 8080, renames `dev` to `device`, and grows the `net` block |
| `firmware/s3/main/main.c` | Registers the new endpoints |
| `firmware/s3/main/CMakeLists.txt` | New sources; `esp_http_server`, `nvs_flash` and `json` requirements |
| `firmware/s3/verify-on-host.sh` | The port move, and a `/net` round trip |
| `firmware/s3/README.md` | The port move |
| `tools/test-all.sh` | Runs the dongle's host tests |

---

### Task 1: The two corrections Plan 1 left behind

The spec names both: the dongle's server must vacate port 80 for the car, and its identity key must match the car's spelling. Neither has a consumer outside `firmware/s3/` yet, which is what makes now the cheap moment.

**Files:**
- Modify: `firmware/s3/main/status_api.c`
- Modify: `firmware/s3/verify-on-host.sh`
- Modify: `firmware/s3/README.md`

**Interfaces:**
- Produces: the dongle's HTTP surface on `192.168.7.1:8080`, with `{"device":"ajdongle",…}`. Every later task and plan assumes both.

- [ ] **Step 1: Move the server to port 8080**

In `firmware/s3/main/status_api.c`, find `cfg.server_port = 80;` and replace it, comment included:

```c
    /* 8080, not 80: port 80 belongs to the car. Plan 3 forwards it straight through to
     * the car's own REST surface so that CarHost.port and the car's contract never move —
     * the dongle is the new thing in the system, so the dongle takes the unusual port. */
    cfg.server_port = 8080;
```

- [ ] **Step 2: Rename `dev` to `device`**

In the same file, the status handler's format string. Change the key only — every other byte stays:

```c
    int n = snprintf(body, sizeof(body),
                     "{\"device\":\"ajdongle\",\"fw\":\"%s\",\"idf\":\"%s\",\"usb\":\"up\"}",
                     app->version, app->idf_ver);
```

Add above the handler, if no comment there says it:

```c
/* The identity key is `device`, spelled as the car's contract spells it
 * (contract/car-api.json, device_field). The app's "which device am I talking to" check
 * should not need two spellings for one question. */
```

- [ ] **Step 3: Update the log line the endpoint prints at boot**

Still in `status_api.c`, the line that logs the URL. It must name the port it now serves:

```c
    ESP_LOGI(TAG, "http://%s:8080/status", USB_NET_ADDR);
```

- [ ] **Step 4: Update `verify-on-host.sh`**

Its `GET /status` probe hardcodes the old URL. Find the line containing `http://192.168.7.1/status` and change it to `http://192.168.7.1:8080/status`. Leave every other check exactly as it is — the routing and regression checks are unaffected by the port.

- [ ] **Step 5: Update the README**

In `firmware/s3/README.md`, the acceptance-run section quotes `GET /status`. Change the URL in that quote to carry `:8080`, and add one sentence beneath the quoted block:

```markdown
The dongle answers on `:8080`, not `:80`: port 80 is reserved for the car, which Plan 3
forwards through untouched so its own contract and the app's `CarHost.port` never move.
```

- [ ] **Step 6: Build and confirm nothing else referenced the old shape**

```bash
source tools/env-p4.sh && cd firmware/s3 && idf.py build
```

Then, from the repo root, confirm no stale references remain:

```bash
grep -rn '"dev"' firmware/s3/ --include=*.c --include=*.h
grep -rn '192.168.7.1/status' firmware/s3/
```

Expected: both print nothing. A hit means a caller was missed.

- [ ] **Step 7: Commit**

```bash
git add firmware/s3
git commit -m "fix(s3): vacate port 80 for the car, and spell identity as the car does"
```

---

### Task 2: The pure configuration module, host-tested

**Files:**
- Create: `firmware/s3/main/net_cfg.h`
- Create: `firmware/s3/main/net_cfg.c`
- Create: `firmware/s3/test/test_net_cfg.c`
- Create: `firmware/s3/test/Makefile`
- Modify: `tools/test-all.sh`
- Modify: `.gitignore`

**Interfaces:**
- Produces, and Task 3 consumes exactly these:
  - `typedef struct { char ssid[33]; char password[64]; } net_cfg_t;`
  - `typedef enum { NET_CFG_OK, NET_CFG_SSID_LEN, NET_CFG_PASS_LEN } net_cfg_err_t;`
  - `net_cfg_err_t net_cfg_validate(const char *ssid, const char *password, net_cfg_t *out);`
  - `const char *net_cfg_err_field(net_cfg_err_t e);`
  - `const char *net_cfg_err_msg(net_cfg_err_t e);`
  - `int net_cfg_render_public(const net_cfg_t *cfg, bool configured, char *buf, size_t n);`
  - `int net_cfg_render_stored(const net_cfg_t *cfg, char *buf, size_t n);`
  - `bool net_cfg_equal(const net_cfg_t *a, const net_cfg_t *b);`

This is TDD: the test is written and seen to fail before the implementation exists.

- [ ] **Step 1: Write the header**

`firmware/s3/main/net_cfg.h`:

```c
#ifndef NET_CFG_H
#define NET_CFG_H

#include <stdbool.h>
#include <stddef.h>

/* The network the dongle has been told to join.
 *
 * Pure: no ESP-IDF, no cJSON, no NVS, so the rules below are host-tested with plain `cc`
 * rather than reasoned about. net_api.c does the JSON extraction and the flash writing
 * around this module and holds no rules of its own.
 *
 * The SSID is an opaque string. This firmware does not know what a car is and must not
 * learn: the value arrives over the wire and is stored and replayed unread. */

/* WPA2's limits, not ours. 32 bytes is the maximum SSID length; a PSK shorter than 8
 * characters cannot be used, and an empty password means an open network. */
#define NET_SSID_MAX   32
#define NET_PASS_MIN    8
#define NET_PASS_MAX   63

typedef struct {
    char ssid[NET_SSID_MAX + 1];
    char password[NET_PASS_MAX + 1];
} net_cfg_t;

typedef enum {
    NET_CFG_OK = 0,
    NET_CFG_SSID_LEN,
    NET_CFG_PASS_LEN,
} net_cfg_err_t;

/* Validate and copy. `*out` is written only on NET_CFG_OK; a rejected body leaves the
 * caller's stored configuration untouched, which is why validation happens before any
 * flash write rather than during it.
 *
 * Values are rejected, never clamped — the car's domains behave the same way, and a
 * silently truncated SSID would fail to associate with no visible cause. */
net_cfg_err_t net_cfg_validate(const char *ssid, const char *password, net_cfg_t *out);

/* Which field a rejection blames, for the {"error":…,"field":…} reply shape.
 * "" when the body as a whole is at fault. */
const char *net_cfg_err_field(net_cfg_err_t e);

/* The message that accompanies it. */
const char *net_cfg_err_msg(net_cfg_err_t e);

/* The GET /net body. NEVER contains the password: the app holds that value itself and
 * has no use for reading it back, so an endpoint that returns a stored credential would
 * be a liability with nothing on the other side of the trade.
 *
 * Returns the length written, or -1 if buf is too small. */
int net_cfg_render_public(const net_cfg_t *cfg, bool configured, char *buf, size_t n);

/* The NVS body. Contains the password — it has to, since this is what the dongle reloads
 * at boot to rejoin without being told again. Returns the length written, or -1. */
int net_cfg_render_stored(const net_cfg_t *cfg, char *buf, size_t n);

/* Whether two configurations are the same, for the dirty check that keeps an unchanged
 * POST from rewriting flash. */
bool net_cfg_equal(const net_cfg_t *a, const net_cfg_t *b);

#endif /* NET_CFG_H */
```

- [ ] **Step 2: Write the failing test**

`firmware/s3/test/test_net_cfg.c`:

```c
#include "../main/net_cfg.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_accepts_a_normal_network(void) {
    net_cfg_t c;
    assert(net_cfg_validate("AJMiddleCar", "drive1234", &c) == NET_CFG_OK);
    assert(strcmp(c.ssid, "AJMiddleCar") == 0);
    assert(strcmp(c.password, "drive1234") == 0);
}

static void test_accepts_an_open_network(void) {
    /* An empty password is a valid open network, which the car's identity.h already
       contemplates — not a missing field. */
    net_cfg_t c;
    assert(net_cfg_validate("Open", "", &c) == NET_CFG_OK);
    assert(c.password[0] == '\0');
}

static void test_ssid_bounds(void) {
    net_cfg_t c;
    char max[NET_SSID_MAX + 1];
    memset(max, 'a', NET_SSID_MAX);
    max[NET_SSID_MAX] = '\0';
    assert(net_cfg_validate(max, "drive1234", &c) == NET_CFG_OK);

    char over[NET_SSID_MAX + 2];
    memset(over, 'a', NET_SSID_MAX + 1);
    over[NET_SSID_MAX + 1] = '\0';
    assert(net_cfg_validate(over, "drive1234", &c) == NET_CFG_SSID_LEN);

    assert(net_cfg_validate("", "drive1234", &c) == NET_CFG_SSID_LEN);
}

static void test_password_bounds(void) {
    net_cfg_t c;
    assert(net_cfg_validate("net", "1234567", &c) == NET_CFG_PASS_LEN);   /* 7, one short */
    assert(net_cfg_validate("net", "12345678", &c) == NET_CFG_OK);        /* 8, the floor */

    char max[NET_PASS_MAX + 1];
    memset(max, 'p', NET_PASS_MAX);
    max[NET_PASS_MAX] = '\0';
    assert(net_cfg_validate("net", max, &c) == NET_CFG_OK);

    char over[NET_PASS_MAX + 2];
    memset(over, 'p', NET_PASS_MAX + 1);
    over[NET_PASS_MAX + 1] = '\0';
    assert(net_cfg_validate("net", over, &c) == NET_CFG_PASS_LEN);
}

static void test_a_rejected_body_does_not_write_out(void) {
    /* The caller's stored configuration must survive a bad POST intact. */
    net_cfg_t c;
    assert(net_cfg_validate("keep", "drive1234", &c) == NET_CFG_OK);
    assert(net_cfg_validate("", "drive1234", &c) == NET_CFG_SSID_LEN);
    assert(strcmp(c.ssid, "keep") == 0);
}

static void test_errors_name_their_field(void) {
    assert(strcmp(net_cfg_err_field(NET_CFG_SSID_LEN), "ssid") == 0);
    assert(strcmp(net_cfg_err_field(NET_CFG_PASS_LEN), "password") == 0);
    assert(strcmp(net_cfg_err_field(NET_CFG_OK), "") == 0);
    assert(net_cfg_err_msg(NET_CFG_SSID_LEN)[0] != '\0');
    assert(net_cfg_err_msg(NET_CFG_PASS_LEN)[0] != '\0');
}

static void test_public_render_never_leaks_the_password(void) {
    net_cfg_t c;
    assert(net_cfg_validate("AJMiddleCar", "drive1234", &c) == NET_CFG_OK);

    char buf[128];
    int n = net_cfg_render_public(&c, true, buf, sizeof(buf));
    assert(n > 0 && (size_t)n == strlen(buf));
    assert(strstr(buf, "drive1234") == NULL);
    assert(strstr(buf, "\"ssid\":\"AJMiddleCar\"") != NULL);
    assert(strstr(buf, "\"configured\":true") != NULL);
}

static void test_public_render_when_unconfigured(void) {
    net_cfg_t c = { .ssid = "", .password = "" };
    char buf[128];
    assert(net_cfg_render_public(&c, false, buf, sizeof(buf)) > 0);
    assert(strstr(buf, "\"ssid\":\"\"") != NULL);
    assert(strstr(buf, "\"configured\":false") != NULL);
}

static void test_stored_render_round_trips(void) {
    net_cfg_t c;
    assert(net_cfg_validate("AJMiddleCar", "drive1234", &c) == NET_CFG_OK);
    char buf[192];
    assert(net_cfg_render_stored(&c, buf, sizeof(buf)) > 0);
    /* The stored form is what the dongle reloads to rejoin unaided, so it must carry
       the password that the public form must not. */
    assert(strstr(buf, "drive1234") != NULL);
    assert(strstr(buf, "AJMiddleCar") != NULL);
}

static void test_render_refuses_a_small_buffer(void) {
    net_cfg_t c;
    assert(net_cfg_validate("AJMiddleCar", "drive1234", &c) == NET_CFG_OK);
    char tiny[8];
    assert(net_cfg_render_public(&c, true, tiny, sizeof(tiny)) == -1);
    assert(net_cfg_render_stored(&c, tiny, sizeof(tiny)) == -1);
}

static void test_equal_drives_the_dirty_check(void) {
    net_cfg_t a, b;
    assert(net_cfg_validate("net", "drive1234", &a) == NET_CFG_OK);
    assert(net_cfg_validate("net", "drive1234", &b) == NET_CFG_OK);
    assert(net_cfg_equal(&a, &b));
    assert(net_cfg_validate("net", "drive9999", &b) == NET_CFG_OK);
    assert(!net_cfg_equal(&a, &b));
    assert(net_cfg_validate("other", "drive1234", &b) == NET_CFG_OK);
    assert(!net_cfg_equal(&a, &b));
}

int main(void) {
    test_accepts_a_normal_network();
    test_accepts_an_open_network();
    test_ssid_bounds();
    test_password_bounds();
    test_a_rejected_body_does_not_write_out();
    test_errors_name_their_field();
    test_public_render_never_leaks_the_password();
    test_public_render_when_unconfigured();
    test_stored_render_round_trips();
    test_render_refuses_a_small_buffer();
    test_equal_drives_the_dirty_check();
    printf("test_net_cfg: all passed\n");
    return 0;
}
```

- [ ] **Step 3: Write the test Makefile**

`firmware/s3/test/Makefile`, modelled on `firmware/p4/test/Makefile`:

```make
CC = cc
CFLAGS = -I../main -Wall -Wextra -Werror -std=c11

all: test_net_cfg

test_net_cfg: test_net_cfg.c ../main/net_cfg.c ../main/net_cfg.h
	$(CC) $(CFLAGS) -o $@ test_net_cfg.c ../main/net_cfg.c

run: all
	./test_net_cfg

clean:
	rm -f test_net_cfg
```

- [ ] **Step 4: Run the test and watch it fail**

```bash
make -C firmware/s3/test run
```

Expected: a compile failure — `net_cfg.c` does not exist yet. That is the RED step; do not skip past it, and record the message in your report.

- [ ] **Step 5: Write the implementation**

`firmware/s3/main/net_cfg.c`:

```c
#include "net_cfg.h"

#include <stdio.h>
#include <string.h>

net_cfg_err_t net_cfg_validate(const char *ssid, const char *password, net_cfg_t *out)
{
    /* Defensive: the one caller checks cJSON_IsString first, so neither can be NULL in
     * practice. Each blames its own field anyway — an error that names the wrong one
     * costs more to debug than the branch costs to write. */
    if (ssid == NULL) {
        return NET_CFG_SSID_LEN;
    }
    if (password == NULL) {
        return NET_CFG_PASS_LEN;
    }

    size_t sn = strlen(ssid);
    if (sn < 1 || sn > NET_SSID_MAX) {
        return NET_CFG_SSID_LEN;
    }

    size_t pn = strlen(password);
    if (pn != 0 && (pn < NET_PASS_MIN || pn > NET_PASS_MAX)) {
        return NET_CFG_PASS_LEN;
    }

    /* Written last, and only here: a rejected body must leave the caller's stored
     * configuration exactly as it was. */
    memcpy(out->ssid, ssid, sn + 1);
    memcpy(out->password, password, pn + 1);
    return NET_CFG_OK;
}

const char *net_cfg_err_field(net_cfg_err_t e)
{
    switch (e) {
    case NET_CFG_SSID_LEN: return "ssid";
    case NET_CFG_PASS_LEN: return "password";
    case NET_CFG_OK:       break;
    }
    return "";
}

const char *net_cfg_err_msg(net_cfg_err_t e)
{
    switch (e) {
    case NET_CFG_SSID_LEN: return "ssid must be 1..32 bytes";
    case NET_CFG_PASS_LEN: return "password must be empty or 8..63 bytes";
    case NET_CFG_OK:       break;
    }
    return "";
}

int net_cfg_render_public(const net_cfg_t *cfg, bool configured, char *buf, size_t n)
{
    int w = snprintf(buf, n, "{\"ssid\":\"%s\",\"configured\":%s}",
                     cfg->ssid, configured ? "true" : "false");
    /* snprintf returns what it WOULD have written, so this catches truncation as well
     * as failure — a half-written body is worse than a refusal. */
    if (w < 0 || (size_t)w >= n) {
        return -1;
    }
    return w;
}

int net_cfg_render_stored(const net_cfg_t *cfg, char *buf, size_t n)
{
    int w = snprintf(buf, n, "{\"ssid\":\"%s\",\"password\":\"%s\"}",
                     cfg->ssid, cfg->password);
    if (w < 0 || (size_t)w >= n) {
        return -1;
    }
    return w;
}

bool net_cfg_equal(const net_cfg_t *a, const net_cfg_t *b)
{
    return strcmp(a->ssid, b->ssid) == 0 && strcmp(a->password, b->password) == 0;
}
```

- [ ] **Step 6: Run the test and watch it pass**

```bash
make -C firmware/s3/test run
```

Expected: `test_net_cfg: all passed`, with no compiler warnings — the flags include `-Werror`, so a warning is a failure.

- [ ] **Step 7: Wire it into the project's test suite**

In `tools/test-all.sh`, under the `== firmware host tests ==` heading, add the dongle beside the car:

```bash
make -C firmware/p4/test run
make -C firmware/s3/test run
```

And in `.gitignore`, beside the existing compiled-test entries for the car:

```
firmware/s3/test/test_*
!firmware/s3/test/test_*.c
```

- [ ] **Step 8: Run the whole suite**

```bash
tools/test-all.sh
```

Expected: `== all green ==`, now with the dongle's test in the run.

- [ ] **Step 9: Commit**

```bash
git add firmware/s3 tools/test-all.sh .gitignore
git commit -m "feat(s3): the network config domain, validated where it can be tested"
```

---

### Task 3: `GET` and `POST /net`

**Files:**
- Create: `firmware/s3/main/api_util.h`
- Create: `firmware/s3/main/api_util.c`
- Create: `firmware/s3/main/net_api.h`
- Create: `firmware/s3/main/net_api.c`
- Modify: `firmware/s3/main/status_api.h`
- Modify: `firmware/s3/main/status_api.c`
- Modify: `firmware/s3/main/main.c`
- Modify: `firmware/s3/main/CMakeLists.txt`

**Interfaces:**
- Consumes from Task 2: `net_cfg_t`, `net_cfg_validate`, `net_cfg_err_field`, `net_cfg_err_msg`, `net_cfg_render_public`, `net_cfg_render_stored`, `net_cfg_equal`.
- Produces:
  - `esp_err_t api_reply_error(httpd_req_t *req, const char *status, const char *field, const char *msg);`
  - `esp_err_t api_reply_ok(httpd_req_t *req);`
  - `int api_read_body(httpd_req_t *req, char *buf, size_t n);`
  - `esp_err_t net_api_register(httpd_handle_t server);`
  - `void net_api_load(void);` — reads NVS at boot; call before registering.
  - `bool net_api_current(net_cfg_t *out);` — the live configuration; returns whether one is set. Task 4 consumes this, and Plan 3's radio will too.
  - `httpd_handle_t status_api_server(void);` — the running server, so `net_api_register` can attach to it rather than start a second one. This is the accessor Plan 1 deliberately omitted for want of a caller; it now has one.

- [ ] **Step 1: Write `api_util.h`**

```c
#ifndef API_UTIL_H
#define API_UTIL_H

#include <stddef.h>
#include "esp_err.h"
#include "esp_http_server.h"

/* The REST surface's shared plumbing: one error shape for every endpoint, and one body
 * reader that copes with a body TCP split across segments.
 *
 * A deliberate twin of firmware/p4/main/api_util.{c,h} rather than a shared file. The two
 * firmwares do not reference each other — that independence is what lets the dongle stay
 * ignorant of the car — and the price of it is this much duplication, paid knowingly. */

/* {"error":"<msg>","field":"<field>"} with `status` as the HTTP status line.
 * `field` is "" when the body as a whole is at fault. */
esp_err_t api_reply_error(httpd_req_t *req, const char *status, const char *field,
                          const char *msg);

/* {"ok":true} with the JSON content type — the documented success body. */
esp_err_t api_reply_ok(httpd_req_t *req);

/* Read the whole body into buf (NUL-terminated). Returns the length, or -1 when the body
 * is absent, too long for buf, or the socket gave up. */
int api_read_body(httpd_req_t *req, char *buf, size_t n);

#endif /* API_UTIL_H */
```

- [ ] **Step 2: Write `api_util.c`**

```c
#include "api_util.h"

#include <stdio.h>
#include <string.h>

esp_err_t api_reply_error(httpd_req_t *req, const char *status, const char *field,
                          const char *msg)
{
    char body[192];
    int n = snprintf(body, sizeof(body), "{\"error\":\"%s\",\"field\":\"%s\"}", msg, field);
    if (n < 0 || (size_t)n >= sizeof(body)) {
        return ESP_FAIL;
    }
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, n);
}

esp_err_t api_reply_ok(httpd_req_t *req)
{
    static const char ok[] = "{\"ok\":true}";
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, ok, sizeof(ok) - 1);
}

int api_read_body(httpd_req_t *req, char *buf, size_t n)
{
    if (req->content_len <= 0 || (size_t)req->content_len >= n) {
        return -1;
    }
    /* One recv can return a prefix: a body split across TCP segments must be gathered,
     * not truncated into a 400 that blames the field names. */
    size_t got = 0;
    while (got < (size_t)req->content_len) {
        int r = httpd_req_recv(req, buf + got, (size_t)req->content_len - got);
        if (r <= 0) {
            return -1;
        }
        got += (size_t)r;
    }
    buf[got] = '\0';
    return (int)got;
}
```

- [ ] **Step 3: Expose the running server from `status_api`**

In `firmware/s3/main/status_api.h`, add the accessor beside `status_api_start`:

```c
/* The running server, so another module can register its handlers on it rather than
 * start a second one. NULL before status_api_start succeeds. */
httpd_handle_t status_api_server(void);
```

The header will need `#include "esp_http_server.h"` for the type. In `status_api.c`, add the definition beside the existing static:

```c
httpd_handle_t status_api_server(void)
{
    return s_server;
}
```

Also raise the handler budget, since two more are arriving. Find `cfg.max_uri_handlers` and set it to `6`, with the reason:

```c
    /* /status, GET /net, POST /net, and room for Plan 3's additions — sized so that a
     * new endpoint is not also a config change. */
    cfg.max_uri_handlers = 6;
```

- [ ] **Step 4: Write `net_api.h`**

```c
#ifndef NET_API_H
#define NET_API_H

#include <stdbool.h>
#include "esp_err.h"
#include "esp_http_server.h"
#include "net_cfg.h"

/* GET /net  → {"ssid":"…","configured":true|false}   — never the password
 * POST /net ← {"ssid":"…","password":"…"}            → {"ok":true}
 *
 * The SSID is opaque here. This firmware does not know what a car is. */

/* Load the stored configuration from NVS. Call once at boot, before registering. */
void net_api_load(void);

/* Register both handlers on an already-running server. */
esp_err_t net_api_register(httpd_handle_t server);

/* The live configuration. Returns false when none has been set, in which case *out is
 * left untouched. Plan 3's radio reads this to know what to join. */
bool net_api_current(net_cfg_t *out);

#endif /* NET_API_H */
```

- [ ] **Step 5: Write `net_api.c`**

```c
#include "net_api.h"

#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "api_util.h"

static const char *TAG = "net_api";

/* One JSON string under one key, which is how the car stores each of its five config
 * domains. The namespace is the dongle's own — nothing here shares storage with anything. */
static const char NVS_NAMESPACE[] = "dongle";
static const char NVS_KEY[] = "net";

static net_cfg_t s_cfg;
static bool s_configured;

bool net_api_current(net_cfg_t *out)
{
    if (!s_configured) {
        return false;
    }
    *out = s_cfg;
    return true;
}

/* Persist, unless the stored bytes already say this. The dirty check is the point: an app
 * that POSTs its configuration unconditionally at every launch — which is exactly what the
 * app does — must not erase a flash sector each time. */
static esp_err_t store(const net_cfg_t *cfg)
{
    char json[192];
    if (net_cfg_render_stored(cfg, json, sizeof(json)) < 0) {
        return ESP_FAIL;
    }

    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h), TAG, "cannot open nvs");

    char existing[192];
    size_t len = sizeof(existing);
    if (nvs_get_str(h, NVS_KEY, existing, &len) == ESP_OK && strcmp(existing, json) == 0) {
        nvs_close(h);
        return ESP_OK;
    }

    esp_err_t err = nvs_set_str(h, NVS_KEY, json);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

void net_api_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;                      /* nothing stored yet — the first boot */
    }

    char json[192];
    size_t len = sizeof(json);
    esp_err_t err = nvs_get_str(h, NVS_KEY, json, &len);
    nvs_close(h);
    if (err != ESP_OK) {
        return;
    }

    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        ESP_LOGW(TAG, "stored config is not JSON; ignoring it");
        return;
    }
    const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(root, "ssid");
    const cJSON *pass = cJSON_GetObjectItemCaseSensitive(root, "password");
    if (cJSON_IsString(ssid) && cJSON_IsString(pass) &&
        net_cfg_validate(ssid->valuestring, pass->valuestring, &s_cfg) == NET_CFG_OK) {
        s_configured = true;
        ESP_LOGI(TAG, "network configured: %s", s_cfg.ssid);
    } else {
        /* Revalidated rather than trusted: bounds can move between firmware versions, and
         * a stored value that no longer passes must not reach the radio. */
        ESP_LOGW(TAG, "stored config failed validation; ignoring it");
    }
    cJSON_Delete(root);
}

static esp_err_t net_get(httpd_req_t *req)
{
    char body[128];
    int n = net_cfg_render_public(&s_cfg, s_configured, body, sizeof(body));
    if (n < 0) {
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, n);
}

static esp_err_t net_post(httpd_req_t *req)
{
    char raw[256];
    int len = api_read_body(req, raw, sizeof(raw));
    if (len < 0) {
        return api_reply_error(req, "400 Bad Request", "", "body missing or too long");
    }

    cJSON *root = cJSON_Parse(raw);
    if (root == NULL) {
        return api_reply_error(req, "400 Bad Request", "", "body is not JSON");
    }

    const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(root, "ssid");
    const cJSON *pass = cJSON_GetObjectItemCaseSensitive(root, "password");
    if (!cJSON_IsString(ssid) || !cJSON_IsString(pass)) {
        cJSON_Delete(root);
        return api_reply_error(req, "400 Bad Request", "",
                               "ssid and password are both required strings");
    }

    net_cfg_t next;
    net_cfg_err_t verr = net_cfg_validate(ssid->valuestring, pass->valuestring, &next);
    cJSON_Delete(root);
    if (verr != NET_CFG_OK) {
        return api_reply_error(req, "400 Bad Request", net_cfg_err_field(verr),
                               net_cfg_err_msg(verr));
    }

    if (s_configured && net_cfg_equal(&s_cfg, &next)) {
        return api_reply_ok(req);    /* nothing changed: no flash write, no radio churn */
    }

    if (store(&next) != ESP_OK) {
        return api_reply_error(req, "500 Internal Server Error", "", "cannot persist");
    }
    s_cfg = next;
    s_configured = true;
    ESP_LOGI(TAG, "network set: %s", s_cfg.ssid);
    return api_reply_ok(req);
}

esp_err_t net_api_register(httpd_handle_t server)
{
    static const httpd_uri_t get_uri = {
        .uri = "/net", .method = HTTP_GET, .handler = net_get,
    };
    static const httpd_uri_t post_uri = {
        .uri = "/net", .method = HTTP_POST, .handler = net_post,
    };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &get_uri), TAG,
                        "cannot register GET /net");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &post_uri), TAG,
                        "cannot register POST /net");
    return ESP_OK;
}
```

- [ ] **Step 6: Call it from `main.c`**

Add `#include "net_api.h"` beside the existing includes, and after `status_api_start()`:

```c
    ESP_ERROR_CHECK(status_api_start());
    net_api_load();
    ESP_ERROR_CHECK(net_api_register(status_api_server()));
    ESP_LOGI(TAG, "dongle up");
```

`net_api_load` runs before registration so a request arriving the instant the handler exists already sees the stored value.

- [ ] **Step 7: Add the sources and the cJSON requirement**

`firmware/s3/main/CMakeLists.txt`:

```cmake
idf_component_register(SRCS "main.c" "usb_net.c" "status_api.c" "api_util.c" "net_api.c" "net_cfg.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES nvs_flash esp_netif esp_event esp_http_server esp_app_format json)
```

`json` is the component that provides cJSON.

- [ ] **Step 8: Build**

```bash
source tools/env-p4.sh && cd firmware/s3 && idf.py build
```

Expected: success with no warnings. Then `tools/test-all.sh` — still green; this task adds no host tests because everything it introduces is IDF glue around the module Task 2 already covers.

- [ ] **Step 9: Commit**

```bash
git add firmware/s3
git commit -m "feat(s3): GET and POST /net, persisted without needless flash writes"
```

---

### Task 4: `/status` reports the configured network

**Files:**
- Modify: `firmware/s3/main/status_api.c`

**Interfaces:**
- Consumes from Task 3: `net_api_current(net_cfg_t *out)`.

- [ ] **Step 1: Extend the status body**

Add `#include "net_api.h"` to `status_api.c`, and replace the body construction in the handler:

```c
static esp_err_t status_get(httpd_req_t *req)
{
    const esp_app_desc_t *app = esp_app_get_description();

    net_cfg_t cfg;
    const char *ssid = net_api_current(&cfg) ? cfg.ssid : "";

    /* `state` is always "idle" in this firmware, and honestly so: there is no radio yet,
     * so there is nothing that could be joining, connected or failed. The field is here
     * rather than added later because its SHAPE is final — Plan 3 gives it the other
     * values without moving a key or changing a caller. */
    char body[256];
    int n = snprintf(body, sizeof(body),
                     "{\"device\":\"ajdongle\",\"fw\":\"%s\",\"idf\":\"%s\",\"usb\":\"up\","
                     "\"net\":{\"ssid\":\"%s\",\"state\":\"idle\",\"rssi\":0}}",
                     app->version, app->idf_ver, ssid);
    if (n < 0 || (size_t)n >= sizeof(body)) {
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, n);
}
```

Note the buffer grew from 192 to 256: the `net` block adds up to ~60 bytes, and the SSID up to 32 of them.

- [ ] **Step 2: Build and check the size claim holds**

```bash
source tools/env-p4.sh && cd firmware/s3 && idf.py build
```

Then satisfy yourself on paper that the buffer cannot truncate: the fixed template is 118 bytes, `version` and `idf_ver` are `char[32]` in `esp_app_desc_t` (31 usable each), and the SSID is at most 32 — 118 + 31 + 31 + 32 = 212 of 256. Record that arithmetic in your report; the truncation branch is unreachable and should be shown to be, not assumed.

- [ ] **Step 3: Commit**

```bash
git add firmware/s3
git commit -m "feat(s3): /status reports the network the dongle was told to join"
```

---

### Task 5: On the bench

The endpoints are exercisable with nothing but a laptop and `curl`, which is the property the plan was cut to preserve. This task spends it.

**Files:**
- Modify: `firmware/s3/verify-on-host.sh`
- Modify: `firmware/s3/README.md`

- [ ] **Step 1: Add a `/net` round trip to the verification script**

In `firmware/s3/verify-on-host.sh`, inside the attached block after the `GET /status` probe, add:

```bash
      echo "GET /net (before):"
      curl -s --max-time 5 http://192.168.7.1:8080/net 2>&1 | sed 's/^/  /'
      echo
      echo "POST /net (a network that does not exist — nothing here joins it):"
      curl -s --max-time 5 -X POST http://192.168.7.1:8080/net \
           -H 'Content-Type: application/json' \
           -d '{"ssid":"BenchTest","password":"benchpass"}' 2>&1 | sed 's/^/  /'
      echo
      echo "GET /net (after — must show BenchTest, must NOT show the password):"
      curl -s --max-time 5 http://192.168.7.1:8080/net 2>&1 | sed 's/^/  /'
      echo
      echo "POST /net with a 3-character password (must be refused, naming the field):"
      curl -s --max-time 5 -X POST http://192.168.7.1:8080/net \
           -H 'Content-Type: application/json' \
           -d '{"ssid":"BenchTest","password":"abc"}' 2>&1 | sed 's/^/  /'
      echo
```

- [ ] **Step 2: Flash and run it**

Flash over the `COM` port, move the cable to `USB`, and run:

```bash
firmware/s3/verify-on-host.sh /tmp/dongle-p2.log && cat /tmp/dongle-p2.log
```

What must be true, and what each failure would mean:

| Check | Expected | If not |
|---|---|---|
| `GET /net` before | `{"ssid":"","configured":false}` | a previous bench run left NVS populated — erase it or accept the seeded value |
| `POST` valid | `{"ok":true}` | read the console over `COM`; `net_api` logs the SSID it accepted |
| `GET /net` after | `BenchTest`, **no** `benchpass` anywhere | a password leak is a Critical defect, not a cosmetic one |
| `POST` short password | `400` with `"field":"password"` | validation is not reaching the handler |
| `/status` | carries the `net` block with `"state":"idle"` | Task 4 did not land |

- [ ] **Step 3: Prove persistence across a reboot**

This is the one property `curl` alone cannot show, and the one NVS exists for:

```bash
# with the cable back on COM
python -m esptool --chip esp32s3 -p /dev/cu.wchusbserial5C840016191 --after hard_reset flash_id
# cable back to USB
curl -s http://192.168.7.1:8080/net
```

Expected: `{"ssid":"BenchTest","configured":true}` — the value survived a power cycle without being told again.

- [ ] **Step 4: Record what the bench answered**

Add rows to the README's hardware table with the date, in the style of the existing ones — the file is the live record of what the hardware has answered, and every row in it is traceable to a measurement:

```markdown
| `POST /net` persists across a reboot | *(record what you observed)* | |
| `GET /net` withholds the password | *(record what you observed)* | |
```

Replace each placeholder with the observation. A check you did not run keeps its placeholder; an untested row must never read as a pass.

- [ ] **Step 5: Commit**

```bash
git add firmware/s3
git commit -m "docs(s3): what the config domain answered on the bench"
```

---

## After this plan

**The dongle's `POST /ota` is not here, and that is a correction to the decomposition rather
than an oversight in this plan.** The spec describes four pieces — config, OTA, radio, and the
app — and OTA belongs with two things that are not in this plan's scope: switching
`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` on, which the spec says must arrive together with the
OTA it protects, and teaching `tools/release.sh` to build and attach `ajdongle.bin` beside the
car's image. It is its own plan, and like this one it needs nothing but a laptop.

Plan 3 brings up the radio, and inherits three things this plan deliberately left it:

1. **The guard on the configuration surface.** `httpd_start` binds `INADDR_ANY`, and `status_api.h` says so at the declaration. The moment a station exists, `POST /net` — which now carries a password — answers on the car's network too. `getsockname` on `httpd_req_to_sockfd`, or `httpd_config_t.open_fn`, and the spec argues for the second.
2. **`net.state`'s other values.** `net_api_current()` tells the radio what to join; `joining`, `connected` and `failed` replace the constant `idle`, and `rssi` stops being zero.
3. **The image-size question.** 395 KB today in a 4 MB slot. `esp_wifi` and a station will roughly double it — comfortable, and worth measuring rather than assuming.
