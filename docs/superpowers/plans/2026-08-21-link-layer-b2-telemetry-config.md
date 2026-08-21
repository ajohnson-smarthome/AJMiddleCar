# Link Layer — Plan B2: make telemetry work, make it cheap, and use the generated table

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Un-break the telemetry channel on the wire the car speaks today, stop paying for it out of a high-priority timer callback, and put the generated config table to work.

**Architecture:** Three separable pieces. `ws_control` learns to capture its client on the data path, because ESP-IDF 6.0.2 does not call the URI handler on the WebSocket handshake at all — the reason the 5 Hz push has never sent a single frame on this board. `telemetry` stops doing an NVS read and a 5 s-timeout SDIO RPC inside an `esp_timer` callback. And the five hand-copied config endpoints collapse into one handler driven by `cfg_table.inc`, which Plan A generated and nothing has consumed yet.

**Tech Stack:** C11, ESP-IDF 6.0.2, FreeRTOS, cJSON. Host tests with plain `cc` under `-Wall -Wextra -Werror`; generator tests with stdlib `unittest`.

**Spec:** `docs/superpowers/specs/2026-08-21-link-layer-rearchitecture.md`

## Global Constraints

- The `/ws` channel stays WebSocket in this plan. B3 replaces it with UDP; B2 makes it work in the meantime, because a board that cannot report telemetry cannot be observed while B3 is built.
- REST responses become `application/json`. The app checks only `status == 200`, so this is compatible — but it is a wire change and must be reflected in `docs/protocol.md`.
- Ranges and defaults come **only** from `contract/car-api.json`. No range literal may appear in a config handler after this plan.
- `tools/test-all.sh` green and `idf.py build` clean after every task.
- Build baseline entering this plan: `ajmiddlecar.bin` = 0xbb620 bytes, 82% free.

## File Structure

| File | Responsibility |
|---|---|
| `firmware/p4/main/ws_control.c` | Captures the client fd on the data path, validates it before pushing, drains frames it declines |
| `firmware/p4/main/telemetry.{c,h}` | Cheap gather: cached `calibrated`, RSSI sampled off the timer, per-consumer frame-rate accumulators |
| `firmware/p4/main/calibration.{c,h}` | Gains a cached `calibration_is_valid()` so telemetry stops reading flash at 5 Hz |
| `firmware/p4/main/cfg_api.{c,h}` | **New.** One handler pair for all five config domains, driven by `cfg_table.inc` |
| `firmware/p4/main/{ramp,trim,recovery,wheel,dims}_api.{c,h}` | Deleted. Their bindings move into `cfg_api.c` as a small table of getters and setters |
| `firmware/p4/main/pca9685.{c,h}` | Bus-fault escalation: reset the bus after repeated failures |
| `docs/protocol.md` | JSON responses, and the `/ws` client rule stated as the firmware actually implements it |

---

### Task 1: Capture the WebSocket client where IDF 6.0.2 actually calls us

`ws_control.c:19` captures the client socket in the `req->method == HTTP_GET` branch — the handshake. Under ESP-IDF 5.4 that branch ran, because the handshake block fell through to the handler. Under 6.0.2 it does not:

```c
/* If the request is websocket handshake, then do not call the uri->handler */
return ESP_OK;
```
— `components/esp_http_server/src/httpd_uri.c:362`

and on data frames `httpd_parse.c:698` sets `r->method = 0`, which is not `HTTP_GET` either. So `s_client_fd` is `-1` for the life of the process, `ws_control_send()` returns on its first line, and **the 5 Hz telemetry push has never sent a frame on this board**. The app's whole liveness model rests on those frames, which is why it drops a "searching" overlay over the controls a second after connecting.

Two more defects in the same function ride along. A send error clears `s_client_fd` permanently even for `EAGAIN`, which IDF itself classifies as retryable — one slow moment and telemetry is dead until the client reconnects. And a frame the handler declines (oversized, or a type it ignores) is never read out of the socket, so its payload is parsed as the next frame's header and the stream desynchronises.

**Files:**
- Modify: `firmware/p4/main/ws_control.c`

**Interfaces:**
- Produces: `ws_control_send` unchanged in signature; `s_client_fd` maintained from the data path.

- [ ] **Step 1: Capture the client on every accepted frame**

Replace the `HTTP_GET` branch at the top of `ws_handler` with a comment recording why it is gone, and capture after a successful parse instead:

```c
static esp_err_t ws_handler(httpd_req_t *req) {
    /* No HTTP_GET branch here on purpose. ESP-IDF 6.0.2 does not call the URI handler
       for a WebSocket handshake at all (esp_http_server/src/httpd_uri.c: "if the request
       is websocket handshake, then do not call the uri->handler"), and on data frames it
       sets req->method to 0. Capturing the client on the handshake — which is what 5.4
       allowed and what this file used to do — left s_client_fd at -1 forever, so the
       telemetry push never sent anything. The client is captured below instead, on the
       first frame it sends, which is also the honest expression of "last connect wins". */
```

and after `control_parse_json` succeeds, before the drive call:

```c
        int fd = httpd_req_to_sockfd(req);
        if (fd != s_client_fd) {
            ESP_LOGI(TAG, "ws client fd=%d (was %d)", fd, s_client_fd);
            s_client_fd = fd;
        }
```

- [ ] **Step 2: Drain a frame the handler declines**

Both early returns after the length probe currently leave the payload unread. Replace:

```c
    if (frame.len == 0 || frame.len > 31) {
        ESP_LOGD(TAG, "ignoring ws frame len=%d", (int)frame.len);
        return ESP_OK;  // ignore empty / oversized frames
    }
```

with a drain that consumes the payload in chunks so the stream stays framed:

```c
    if (frame.len == 0) return ESP_OK;
    if (frame.len > 31) {
        /* Read it out and throw it away. An unread payload is parsed as the next
           frame's header, which desynchronises the stream for good — the connection
           then produces garbage rather than closing, which is much harder to see. */
        uint8_t sink[64];
        size_t want = frame.len;
        while (want > 0) {
            httpd_ws_frame_t chunk = { .type = frame.type };
            size_t n = want < sizeof(sink) ? want : sizeof(sink);
            chunk.payload = sink;
            chunk.len = n;
            if (httpd_ws_recv_frame(req, &chunk, n) != ESP_OK) break;
            want -= n;
        }
        ESP_LOGW(TAG, "dropped an oversized ws frame (%d bytes)", (int)frame.len);
        return ESP_OK;
    }
```

Apply the same drain to the `frame.type` guard above it by moving that check **after** the length is known and reusing the same block — or, simpler and equally correct, leave the type guard where it is and note in a comment that control frames carry no payload the handler must consume (`handle_ws_control_frames` is false, so PING/PONG/CLOSE never reach here).

- [ ] **Step 3: Validate the fd before pushing, and keep a retryable failure**

Replace `ws_control_send`'s body:

```c
esp_err_t ws_control_send(const char *data, size_t len) {
    int fd = s_client_fd;
    if (fd < 0) return ESP_OK;  // no client — nothing to do
    httpd_handle_t server = http_server_get_handle();
    if (server == NULL) return ESP_FAIL;

    /* A socket number is reused the moment it closes, so a stale fd can point at a
       REST connection — and pushing a WebSocket frame into an HTTP response is a
       failure with no symptom. Ask the server what this fd actually is. */
    if (httpd_ws_get_fd_info(server, fd) != HTTPD_WS_CLIENT_WEBSOCKET) {
        s_client_fd = -1;
        return ESP_OK;
    }

    httpd_ws_frame_t frame = {
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)data,
        .len = len,
    };
    esp_err_t e = httpd_ws_send_frame_async(server, fd, &frame);
    /* Deliberately do NOT clear s_client_fd here. A full send buffer is a moment, not
       a disconnection, and clearing on it used to kill telemetry for the life of an
       otherwise healthy socket. A real disconnection is caught by the fd check above. */
    return e;
}
```

- [ ] **Step 4: Build**

```bash
cd ~/VSCode/esp32-p4-car/firmware/p4
source ~/esp/esp-idf-v6.0.2/export.sh >/dev/null 2>&1
idf.py build 2>&1 | grep -E "error:|warning:|Project build complete"
```

Expected: `Project build complete.` with no warnings.

- [ ] **Step 5: Commit**

```bash
cd ~/VSCode/esp32-p4-car
git add firmware/p4/main/ws_control.c
git commit -m "fix(fw): the telemetry push has never sent a frame — capture the client where IDF calls us

ws_control captured the client socket in the req->method == HTTP_GET branch,
which is the WebSocket handshake. ESP-IDF 5.4 called the handler there. 6.0.2
does not: httpd_uri.c returns early with the comment 'if the request is
websocket handshake, then do not call the uri->handler', and on data frames
httpd_parse.c sets method to 0. So s_client_fd stayed -1 for the life of the
process and ws_control_send returned on its first line, every time, since the
port to this board.

The app builds its entire liveness model on those frames, which is why it drops
a searching overlay over the controls a second after connecting and never lifts
it. The client is now captured on the first frame it sends, which is also the
honest expression of 'last connect wins'.

Two more in the same function. A send failure cleared the fd permanently, even
for EAGAIN, which IDF classifies as retryable — one slow moment killed telemetry
for the life of a healthy socket. And a declined frame was never read out of the
socket, so its payload became the next frame's header and the stream
desynchronised into garbage instead of closing.
"
```

---

### Task 2: Stop paying for telemetry out of a priority-22 timer callback

`telemetry_gather` runs in the `esp_timer` task at priority 22, five times a second, and does two things that do not belong there: `calibration_load()` opens NVS and runs `cJSON_Parse`, and `ap_client_rssi()` makes an `esp_wifi_ap_get_sta_list` call that crosses SDIO to the C6 — the same class of RPC whose 5 s timeout cost this project five seconds of every boot before the radio was updated. Blocking that task delays every other `esp_timer` callback in the system, including IDF's own.

Separately, `ws_fps_now()` keeps `static uint32_t last_frames` / `static int64_t last_us` and is called from two tasks — the 5 Hz push and every `/status` request — which read-modify-write each other's measurement interval. The number `docs/protocol.md` calls "a direct measure of the uplink" is currently a function of how the two callers interleave.

**Files:**
- Modify: `firmware/p4/main/telemetry.c`, `firmware/p4/main/telemetry.h`, `firmware/p4/main/calibration.c`, `firmware/p4/main/calibration.h`, `firmware/p4/main/car.c`, `firmware/p4/main/calib_api.c`
- Modify: `firmware/p4/test/test_telemetry.c`

**Interfaces:**
- Produces: `bool calibration_is_valid(void)` — the cached answer, no flash access; `void calibration_set_valid(bool)` — called by `car_init` and `/calib/save`. `telemetry_gather` gains a `consumer` argument: `typedef enum { TELEM_PUSH, TELEM_STATUS } telem_consumer_t;`.

- [ ] **Step 1: Cache the calibrated flag**

In `calibration.h`, add:

```c
// Whether a valid calibration is loaded, answered from RAM. The car reports this at
// 5 Hz, and reading flash and running a JSON parse that often — from a timer callback
// at that — is not a price a status flag is worth.
bool calibration_is_valid(void);
void calibration_set_valid(bool v);
```

In `calibration.c`:

```c
static bool s_valid = false;

bool calibration_is_valid(void) { return s_valid; }
void calibration_set_valid(bool v) { s_valid = v; }
```

and set it inside `calibration_load` on the success path (`s_valid = true;` just before `return true`).

In `car.c`'s `car_init`, the `else` branch that logs "no NVS calibration" also calls `calibration_set_valid(false)`. In `calib_api.c`'s `calib_save`, after a successful `calibration_save`, call `calibration_set_valid(true)`.

Replace `calib_get`'s `calibration_load(&tmp)` with `calibration_is_valid()` as well — a GET of a flag should not read flash either.

- [ ] **Step 2: Sample RSSI off the hot path**

In `telemetry.c`, replace the direct call with a cached value refreshed at most once a second, from the task that is already allowed to block:

```c
/* The AP-side RSSI costs an esp_wifi_ap_get_sta_list, which on this board is an RPC
   across SDIO to the C6 — the same class of call whose timeout used to cost five
   seconds of every boot. It is a display value that changes slowly, so it is sampled
   at 1 Hz from the push task rather than 5 Hz from inside the timer callback. */
static int      s_rssi_cached = 0;
static int64_t  s_rssi_at_us  = 0;

static int ap_client_rssi_cached(void) {
    int64_t now = esp_timer_get_time();
    if (s_rssi_at_us == 0 || now - s_rssi_at_us > 1000000) {
        s_rssi_at_us = now;
        wifi_sta_list_t sta;
        s_rssi_cached = (esp_wifi_ap_get_sta_list(&sta) == ESP_OK && sta.num > 0)
                      ? sta.sta[0].rssi : 0;
    }
    return s_rssi_cached;
}
```

- [ ] **Step 3: Give each consumer its own frame-rate accumulator**

Replace `ws_fps_now()` with a version that keeps one accumulator per consumer:

```c
typedef enum { TELEM_PUSH, TELEM_STATUS, TELEM_CONSUMERS } telem_consumer_t;

/* One accumulator per consumer. Sharing them meant a /status poll consumed the push's
   measurement interval, so the "frames per second" both reported was a function of how
   the two callers interleaved rather than of the uplink. */
static int fps_now(telem_consumer_t who) {
    static uint32_t last_frames[TELEM_CONSUMERS];
    static int64_t  last_us[TELEM_CONSUMERS];
    uint32_t frames = ws_control_frames();
    int64_t now = esp_timer_get_time();
    int fps = 0;
    if (last_us[who] != 0) {
        int64_t dt = now - last_us[who];
        if (dt > 0 && dt < 10 * 1000000LL) {
            fps = (int)(((int64_t)(uint32_t)(frames - last_frames[who]) * 1000000LL) / dt);
        }
    }
    last_frames[who] = frames;
    last_us[who] = now;
    return fps;
}
```

`telemetry_gather` gains the consumer argument and passes it through; `telemetry_json` calls it with `TELEM_PUSH`; `status_api.c` calls it with `TELEM_STATUS`. Update both declarations in `telemetry.h`.

Move the enum into `telemetry.h` above the `TELEMETRY_HOST_TEST` guard so `status_api.c` can name it.

- [ ] **Step 4: Move the push off the esp_timer task**

Replace the `esp_timer` in `telemetry_start` with a plain task, so a blocking send cannot delay IDF's own timers:

```c
static void push_task(void *arg) {
    (void)arg;
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(PUSH_PERIOD_MS));
        char buf[288];
        int n = telemetry_json(buf, sizeof(buf));
        if (n > 0) ws_control_send(buf, (size_t)n);
    }
}

esp_err_t telemetry_start(void) {
    /* A task, not an esp_timer callback. httpd_ws_send_frame_async writes from the
       calling context, so a client with a full receive window blocks it for as long as
       the socket's send timeout — which on the esp_timer task delays every other timer
       in the system, IDF's included. */
    BaseType_t ok = xTaskCreate(push_task, "telemetry", 3072, NULL, 4, NULL);
    if (ok != pdPASS) return ESP_FAIL;
    ESP_LOGI(TAG, "telemetry push started (5 Hz)");
    return ESP_OK;
}
```

with `#define PUSH_PERIOD_MS 200` replacing `PUSH_PERIOD_US`.

- [ ] **Step 5: Update the telemetry host test for the new signature**

`telemetry_fields` is unchanged, so `test_telemetry.c` needs no edit unless the enum moved above the host-test guard breaks its include. Compile it and fix only what actually breaks:

```bash
cd ~/VSCode/esp32-p4-car/firmware/p4/test
cc -I../main -Wall -Wextra -Werror -std=c11 -o /tmp/tt test_telemetry.c -lm && /tmp/tt
```

- [ ] **Step 6: Build and run everything**

```bash
cd ~/VSCode/esp32-p4-car/firmware/p4
source ~/esp/esp-idf-v6.0.2/export.sh >/dev/null 2>&1
idf.py build 2>&1 | grep -E "error:|warning:|Project build complete"
cd ~/VSCode/esp32-p4-car && ./tools/test-all.sh 2>&1 | tail -3
```

- [ ] **Step 7: Commit**

```bash
cd ~/VSCode/esp32-p4-car
git add firmware/p4/main firmware/p4/test
git commit -m "perf(fw): telemetry stops costing an NVS read and an SDIO RPC per frame

telemetry_gather ran in the esp_timer task at priority 22, five times a second,
and opened NVS with a cJSON parse for a boolean while making an
esp_wifi_ap_get_sta_list call that crosses SDIO to the C6. Blocking that task
delays every esp_timer callback in the system, IDF's own included.

calibrated is now a cached flag, RSSI is sampled at 1 Hz, and the push runs on
its own task — httpd_ws_send_frame_async writes from the calling context, so a
client with a full receive window used to block the timer task for a socket
timeout.

ws_fps had one pair of statics shared by the 5 Hz push and every /status
request, so each consumed the other's measurement interval. The number the
protocol doc calls a direct measure of the uplink was a function of how the two
callers interleaved. One accumulator each."
```

---

### Task 3: One config handler, driven by the generated table

Five `*_api.c` files are the same forty lines with different nouns: GET snprintf, POST `httpd_req_recv` into a fixed stack buffer, cJSON parse, hand-written range check, setter, saver, `return httpd_resp_sendstr(req, "ok")`. Every range literal in them duplicates one that now lives in `contract/car-api.json`.

Three defects are shared by all five and fixed once here. The body is read with a single `httpd_req_recv` and no loop, so a body split across TCP segments is truncated and rejected with a message blaming the field names. `HTTPD_SOCK_ERR_TIMEOUT` is not retried. And the reply is the bare string `ok` with content type `text/html`, under a protocol doc that says everything is JSON.

**Files:**
- Create: `firmware/p4/main/cfg_api.c`, `firmware/p4/main/cfg_api.h`
- Delete: `firmware/p4/main/{ramp,trim,recovery,wheel,dims}_api.{c,h}`
- Modify: `firmware/p4/main/CMakeLists.txt`, `firmware/p4/main/main.c`

**Interfaces:**
- Consumes: `CFG_DOMAINS`, `CFG_DOMAIN_COUNT`, `cfg_field_t`, `cfg_domain_t` from `cfg_table.inc` and `cfg_contract.h` (Plan A).
- Produces: `esp_err_t cfg_api_start(void)` registering GET and POST for all five paths.

- [ ] **Step 1: Write the binding table**

The generated table carries the data; the binding to each domain's getter and setter is the one part a schema cannot describe. In `cfg_api.c`, above the handlers:

```c
/* Values are moved as an array of int32 in the field order the generated table
   declares, so the generic handler never needs to know a domain's struct layout. */
typedef void (*cfg_get_fn)(int32_t *out);
typedef void (*cfg_set_fn)(const int32_t *in);
typedef void (*cfg_save_fn)(void);

typedef struct {
    const char *path;
    cfg_get_fn  get;
    cfg_set_fn  set;
    cfg_save_fn save;
} cfg_binding_t;

static void ramp_get_v(int32_t *o) { o[0] = ramp_get_ms(); }
static void ramp_set_v(const int32_t *v) { ramp_set_ms((uint16_t)v[0]); }

static void trim_get_v(int32_t *o) { o[0] = car_get_trim(); }
static void trim_set_v(const int32_t *v) { car_set_trim((int8_t)v[0]); }

static void recover_get_v(int32_t *o) {
    bool en; uint16_t win;
    recovery_get_config(&en, &win);
    o[0] = en ? 1 : 0; o[1] = win;
}
static void recover_set_v(const int32_t *v) {
    recovery_set_config(v[0] != 0, (uint16_t)v[1]);
}

static void wheel_get_v(int32_t *o) {
    wheel_params_t w; wheel_get(&w);
    o[0] = w.diameter_mm; o[1] = w.ppr; o[2] = w.gear_x100; o[3] = w.quad;
}
static void wheel_set_v(const int32_t *v) {
    wheel_params_t w = { (uint16_t)v[0], (uint16_t)v[1], (uint16_t)v[2], (uint8_t)v[3] };
    wheel_set(&w);
}

static void dims_get_v(int32_t *o) {
    dims_params_t d; dims_get(&d);
    o[0] = d.track_mm; o[1] = d.wheelbase_mm;
}
static void dims_set_v(const int32_t *v) {
    dims_params_t d = { (uint16_t)v[0], (uint16_t)v[1] };
    dims_set(&d);
}

static const cfg_binding_t BINDINGS[] = {
    { "/ramp",    ramp_get_v,    ramp_set_v,    ramp_save },
    { "/trim",    trim_get_v,    trim_set_v,    car_save_trim },
    { "/recover", recover_get_v, recover_set_v, recovery_save },
    { "/wheel",   wheel_get_v,   wheel_set_v,   wheel_save },
    { "/dims",    dims_get_v,    dims_set_v,    dims_save },
};
```

- [ ] **Step 2: Write the generic handlers**

```c
static const cfg_domain_t *domain_for(const char *path) {
    for (int i = 0; i < CFG_DOMAIN_COUNT; i++) {
        if (strcmp(CFG_DOMAINS[i].path, path) == 0) return &CFG_DOMAINS[i];
    }
    return NULL;
}

static const cfg_binding_t *binding_for(const char *path) {
    for (size_t i = 0; i < sizeof(BINDINGS) / sizeof(BINDINGS[0]); i++) {
        if (strcmp(BINDINGS[i].path, path) == 0) return &BINDINGS[i];
    }
    return NULL;
}

static esp_err_t reply_error(httpd_req_t *req, const char *status, const char *field,
                             const char *msg) {
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"error\":\"%s\",\"field\":\"%s\"}", msg, field);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

/* Read the whole body, however TCP chose to split it. The old handlers made a single
   httpd_req_recv call, so a body arriving in two segments was silently truncated and
   then rejected with a message blaming the field names. */
static int read_body(httpd_req_t *req, char *buf, size_t n) {
    if (req->content_len <= 0 || (size_t)req->content_len >= n) return -1;
    size_t got = 0;
    int timeouts = 0;
    while (got < (size_t)req->content_len) {
        int r = httpd_req_recv(req, buf + got, (size_t)req->content_len - got);
        if (r > 0) { got += (size_t)r; timeouts = 0; continue; }
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts <= 3) continue;
        return -1;
    }
    buf[got] = '\0';
    return (int)got;
}

static esp_err_t cfg_get(httpd_req_t *req) {
    const cfg_domain_t *d = domain_for(req->uri);
    const cfg_binding_t *b = binding_for(req->uri);
    if (!d || !b) return reply_error(req, "404 Not Found", "", "unknown endpoint");

    int32_t vals[8];
    b->get(vals);

    char buf[192];
    int n = snprintf(buf, sizeof(buf), "{");
    for (int i = 0; i < d->n_fields; i++) {
        const cfg_field_t *f = &d->fields[i];
        const char *sep = i ? "," : "";
        if (f->type == CFG_BOOL) {
            n += snprintf(buf + n, sizeof(buf) - n, "%s\"%s\":%s",
                          sep, f->name, vals[i] ? "true" : "false");
        } else {
            n += snprintf(buf + n, sizeof(buf) - n, "%s\"%s\":%ld",
                          sep, f->name, (long)vals[i]);
        }
        if (n < 0 || (size_t)n >= sizeof(buf)) {
            return reply_error(req, "500 Internal Server Error", "", "response too long");
        }
    }
    snprintf(buf + n, sizeof(buf) - n, "}");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

static esp_err_t cfg_post(httpd_req_t *req) {
    const cfg_domain_t *d = domain_for(req->uri);
    const cfg_binding_t *b = binding_for(req->uri);
    if (!d || !b) return reply_error(req, "404 Not Found", "", "unknown endpoint");

    char body[192];
    if (read_body(req, body, sizeof(body)) < 0) {
        return reply_error(req, "400 Bad Request", "", "body missing or too long");
    }
    cJSON *j = cJSON_Parse(body);
    if (!j) return reply_error(req, "400 Bad Request", "", "malformed JSON");

    int32_t vals[8];
    for (int i = 0; i < d->n_fields; i++) {
        const cfg_field_t *f = &d->fields[i];
        cJSON *it = cJSON_GetObjectItemCaseSensitive(j, f->name);
        if (f->type == CFG_BOOL) {
            if (!cJSON_IsBool(it)) { cJSON_Delete(j); return reply_error(req, "400 Bad Request", f->name, "expected a boolean"); }
            vals[i] = cJSON_IsTrue(it) ? 1 : 0;
            continue;
        }
        if (!cJSON_IsNumber(it)) { cJSON_Delete(j); return reply_error(req, "400 Bad Request", f->name, "expected a number"); }
        int32_t v = (int32_t)it->valueint;
        if (f->type == CFG_ENUM) {
            bool ok = false;
            for (uint8_t k = 0; k < f->n_allowed; k++) if (f->allowed[k] == v) ok = true;
            if (!ok) { cJSON_Delete(j); return reply_error(req, "400 Bad Request", f->name, "not an allowed value"); }
        } else if (v < f->min || v > f->max) {
            cJSON_Delete(j);
            return reply_error(req, "400 Bad Request", f->name, "out of range");
        }
        vals[i] = v;
    }
    cJSON_Delete(j);

    b->set(vals);
    b->save();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}
```

`vals[8]` is sized for the widest domain; assert it at compile time so a sixth field in the schema fails the build rather than the runtime:

```c
_Static_assert(CFG_MAX_FIELDS <= 8, "widen vals[] in cfg_api.c");
```

and have the generator emit `#define CFG_MAX_FIELDS <n>` in `cfg_table.inc` — add that to `emit_c` in `tools/gen_contract.py`, with a matching assertion in `tools/test_gen_contract.py` and in `firmware/p4/test/test_cfg_table.c`.

- [ ] **Step 3: Register, and delete the five files**

```c
esp_err_t cfg_api_start(void) {
    httpd_handle_t server = http_server_get_handle();
    if (server == NULL) { ESP_LOGE(TAG, "http server not started"); return ESP_FAIL; }
    for (int i = 0; i < CFG_DOMAIN_COUNT; i++) {
        httpd_uri_t g = { .uri = CFG_DOMAINS[i].path, .method = HTTP_GET,  .handler = cfg_get };
        httpd_uri_t p = { .uri = CFG_DOMAINS[i].path, .method = HTTP_POST, .handler = cfg_post };
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &g), TAG, "reg GET %s", CFG_DOMAINS[i].path);
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &p), TAG, "reg POST %s", CFG_DOMAINS[i].path);
    }
    ESP_LOGI(TAG, "config endpoints registered (%d domains)", CFG_DOMAIN_COUNT);
    return ESP_OK;
}
```

Delete `ramp_api.{c,h}`, `trim_api.{c,h}`, `recovery_api.{c,h}`, `wheel_api.{c,h}`, `dims_api.{c,h}`; remove them from `CMakeLists.txt` and add `cfg_api.c`; replace the five `*_api_start()` calls in `main.c` with one `cfg_api_start()`.

- [ ] **Step 4: Build and check the handler count**

```bash
cd ~/VSCode/esp32-p4-car/firmware/p4
source ~/esp/esp-idf-v6.0.2/export.sh >/dev/null 2>&1
idf.py build 2>&1 | grep -E "error:|warning:|Project build complete"
grep -rn "max_uri_handlers" main/http_server.c
```

Registered handlers drop from 17 to 15 (`/`, `/ws`, `/calib`×3, `/status`, `/ota`, five domains × 2 = 10 → wait, that is 17 → 17). Count them from the source rather than from this sentence, and update the comment in `http_server.c` to whatever the real number is.

- [ ] **Step 5: Commit**

```bash
cd ~/VSCode/esp32-p4-car
git add -A firmware/p4/main tools/gen_contract.py tools/test_gen_contract.py firmware/p4/test
git commit -m "refactor(fw): one config handler over the generated table

Five *_api.c files were the same forty lines with different nouns, and every
range literal in them duplicated one that now lives in contract/car-api.json.
They are replaced by one handler pair driven by cfg_table.inc, plus a small
binding table — the getters and setters are the one part a schema cannot
describe.

Three defects were shared by all five and are fixed once. The body was read with
a single httpd_req_recv and no loop, so a body split across TCP segments was
truncated and then rejected with a message blaming the field names. A socket
timeout was not retried. And the reply was the bare string ok as text/html,
under a protocol document that says everything is JSON.

Plan A's generated C table now has a consumer."
```

---

### Task 4: Escalate a wedged bus instead of retrying it forever

Carried from B1's review, where it was deliberately deferred rather than invented at review time. Bounding the I2C wait stops the *task* hanging; it does not stop the *motors*, which hold their last duty while `link_task` retries eight channels fifty times a second, forever.

**Files:**
- Modify: `firmware/p4/main/pca9685.c`, `firmware/p4/main/pca9685.h`, `firmware/p4/main/link.c`

- [ ] **Step 1: Expose a bus reset**

In `pca9685.c`:

```c
esp_err_t pca9685_bus_recover(void) {
    /* Clocks the bus until a slave holding SDA low lets go. This is the standard
       recovery for a wedged I2C bus and it is the only lever the firmware has: the
       PCA9685's outputs cannot be commanded while the bus is stuck, so without it
       the motors hold their last duty indefinitely. */
    if (bus_handle == NULL) return ESP_ERR_INVALID_STATE;
    return i2c_master_bus_reset(bus_handle);
}
```

Declare it in `pca9685.h` with the same reasoning in one line.

- [ ] **Step 2: Call it after sustained failure**

In `link.c`'s task, alongside the rate-limited log:

```c
        if (failed) {
            s_fail_ticks++;
            /* Fifty consecutive failing ticks is a second of a bus that is not coming
               back on its own. Try to clock it free; if that does not work, the next
               second tries again. Doing nothing means the motors hold their last duty
               until the battery comes off. */
            if (s_fail_ticks % 50 == 0) {
                ESP_LOGE(TAG, "PCA9685 write failing (%s) — resetting the I2C bus",
                         esp_err_to_name(last_err));
                pca9685_bus_recover();
            }
        } else {
            s_fail_ticks = 0;
        }
```

replacing the rate-limited log block with this (the reset log serves the same purpose at the same rate).

- [ ] **Step 3: Build, test, commit**

```bash
cd ~/VSCode/esp32-p4-car/firmware/p4
source ~/esp/esp-idf-v6.0.2/export.sh >/dev/null 2>&1
idf.py build 2>&1 | grep -E "error:|warning:|Project build complete"
cd ~/VSCode/esp32-p4-car && ./tools/test-all.sh 2>&1 | tail -3
git add firmware/p4/main
git commit -m "fix(fw): a wedged I2C bus gets clocked free instead of retried forever

B1 bounded the wait so the task could not hang, and its review pointed out that
this says nothing about the motors: on a stuck bus they hold their last duty
while the writer retries eight channels fifty times a second, indefinitely.

After a second of unbroken failure the bus is clocked free, which is the standard
recovery for a slave holding SDA low and the only lever the firmware has. It was
deferred out of B1 on purpose — inventing new behaviour during a review is how
unreviewed code ships."
```

---

### Task 5: Say what the wire now does

**Files:**
- Modify: `docs/protocol.md`

- [ ] **Step 1: Correct the REST reply description**

The Configuration section says every response is JSON, which is now true rather than aspirational. Add, after the endpoint table:

```markdown
A successful POST answers `{"ok":true}`. A rejected one answers `4xx` with
`{"error":"…","field":"…"}`, where `field` names the offending key — or is empty when the
fault is with the body as a whole. Both carry `Content-Type: application/json`.
```

- [ ] **Step 2: State the client rule the firmware actually implements**

The Telemetry section says "the socket of the most recent connection wins". Replace with what the code does:

```markdown
One client is served at a time: the car remembers the socket of whichever client most
recently sent a control frame, and pushes telemetry there. It does not close a displaced
socket — a second client can still send commands, and the two will fight. Strict eviction
arrives with the UDP channel, where the sender's address is part of every datagram.
```

- [ ] **Step 3: Verify and commit**

```bash
cd ~/VSCode/esp32-p4-car
bash tools/check_contract.sh
git add docs/protocol.md
git commit -m "docs: JSON replies, and the single-client rule as implemented rather than as wished"
```

---

## Self-Review

**Spec coverage.** This plan implements the spec's "Telemetry gets cheap" and "cfg_api.c — one generic config endpoint" sections, plus the bus-fault escalation carried from B1's review, plus a fix for the dead telemetry channel that the spec addresses only by replacing the transport in B3. It does **not** implement: the UDP channel, `bye`, `proto`, or deleting `ws_control.c` — B3.

**Deliberate deviation from the spec.** The spec assumes `/ws` goes away, so it never says to repair it. Repairing it here is worth one plan's delay: a board whose telemetry has never worked cannot be observed while B3 is built, and every verification step in B3 and D depends on being able to see `rx_fps`, `ctl` and `bus_ok` from the app.

**Placeholders.** None.

**Type consistency.** `calibration_is_valid`/`calibration_set_valid`, `telem_consumer_t`/`TELEM_PUSH`/`TELEM_STATUS`, `cfg_get_fn`/`cfg_set_fn`/`cfg_save_fn`/`cfg_binding_t`, `cfg_api_start`, `pca9685_bus_recover`, `CFG_MAX_FIELDS` — each is spelled identically where defined and where used.

**Known risk.** Task 3 deletes ten files and rewires `main.c`. If `req->uri` ever carries a query string, `domain_for` would fail to match; the app never sends one, and a mismatch produces a clean 404 rather than a wrong write, so it is a correctness gap rather than a hazard. Worth a follow-up if a client ever needs query parameters.
