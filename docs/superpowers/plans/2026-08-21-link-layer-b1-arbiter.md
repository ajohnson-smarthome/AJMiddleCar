# Link Layer — Plan B1: the actuator arbiter and the four safety fixes

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the motors exactly one commander, and stop the four ways the firmware can leave them running when it believes they are stopped.

**Architecture:** A new `link` module owns the actuator end to end — it arbitrates between the five things that want to command it, and it runs the 50 Hz task that is the only writer to the PCA9685. The arbitration itself is a pure state machine in the header, host-tested like `ramp_step` and `watchdog_stale` before it. `ramp` shrinks to the pure slew-rate step plus its NVS-backed setting. `car` keeps mixing and planning, and reads its calibration through an atomic pointer instead of a mutex, so an emergency stop can no longer be dropped on a lock timeout.

**Tech Stack:** C11, ESP-IDF 6.0.2, FreeRTOS. Host tests with plain `cc` under `-Wall -Wextra -Werror`.

**Spec:** `docs/superpowers/specs/2026-08-21-link-layer-rearchitecture.md`

## Global Constraints

- No wire change except **two additive telemetry fields**, `ctl` and `bus_ok`. Additive fields are safe: the app ignores what it does not know.
- The PCA9685 keeps exactly one writer. After this plan that writer lives in `link.c`, and `ramp_set_target` no longer exists as a public symbol.
- Priority order, ascending — a request is granted when its source ranks **at or above** the current owner: `RECOVER(0) < CONSOLE(1) < RT(2) < CALIB(3) < OTA(4) < SAFE(5)`.
- Every pure function stays free of ESP-IDF headers and is host-tested.
- `tools/test-all.sh` must stay green, and `idf.py build` must succeed, after every task that touches firmware.
- Build baseline before this plan: `ajmiddlecar.bin` = 0xbaf00 bytes (764 KB), 82% of the slot free.

## Why that priority order

Each neighbouring pair is a decision, not an accident:

- **RT above RECOVER** — when the driver comes back, the very next control frame must take the wheels back. If the retreat outranked it, control would be refused for up to a tick and the watchdog would not be fed, so the link would look lost while a frame was arriving.
- **CONSOLE above RECOVER** — a bench command must not be silently killed by a retreat. The retreat can only run at all after real `/ws` traffic armed the watchdog, so this costs nothing in a console-only session.
- **RT above CONSOLE** — a live pilot outranks a command typed minutes ago.
- **CALIB above RT** — this is the fix for a real defect: today the wizard's spin pulse is overwritten ~100 ms later by the app's own 10 Hz zero-stream, so the wheel the user is asked to identify barely moves.
- **OTA and SAFE on top** — flashing and an explicit stop answer to nobody.

The watchdog revokes `RT` explicitly (`link_release(LINK_SRC_RT)`) before handing over to recovery, rather than relying on the grant lapsing at the same instant it fires.

## File Structure

| File | Responsibility |
|---|---|
| `firmware/p4/main/link.h` | The pure arbitration state machine (`link_arb_t` + three `static inline` functions), the source enum, and the ESP-side API |
| `firmware/p4/main/link.c` | Owns the arbiter, the 50 Hz writer task, the duty target and shadow. Sole PCA9685 writer |
| `firmware/p4/test/test_link.c` | Host test for the arbitration state machine |
| `firmware/p4/main/ramp.{c,h}` | Shrinks to the pure `ramp_step` plus the NVS-backed `ramp_ms` setting. Loses the task and `ramp_set_target` |
| `firmware/p4/main/pca9685.{c,h}` | Bounded I2C timeout, one retry, and a new `pca9685_zero_all()` |
| `firmware/p4/main/car.{c,h}` | Mixing and planning; calibration read through an atomic pointer; `car_drive`/`car_stop` gain a source |
| `firmware/p4/main/{ws_control,main,calib_api,recovery,ota_api}.c` | Each producer names its source |
| `firmware/p4/main/telemetry.{c,h}` | Two additive fields: `ctl` and `bus_ok` |
| `docs/protocol.md` | Documents the two new telemetry fields |

---

### Task 1: The arbitration state machine, pure and host-tested

**Files:**
- Create: `firmware/p4/main/link.h`
- Create: `firmware/p4/test/test_link.c`
- Modify: `firmware/p4/test/Makefile`

**Interfaces:**
- Produces: `link_src_t {LINK_SRC_NONE = -1, LINK_SRC_RECOVER = 0, LINK_SRC_CONSOLE, LINK_SRC_RT, LINK_SRC_CALIB, LINK_SRC_OTA, LINK_SRC_SAFE}`; `link_arb_t {link_src_t owner; uint32_t until_ms; bool sticky;}`; `bool link_arb_lapsed(const link_arb_t *, uint32_t now)`; `bool link_arb_grant(link_arb_t *, link_src_t src, uint32_t now, uint32_t hold_ms, bool sticky)`; `void link_arb_release(link_arb_t *, link_src_t src)`; `const char *link_src_name(link_src_t)`.

- [ ] **Step 1: Write the failing test**

Create `firmware/p4/test/test_link.c`:

```c
#define LINK_HOST_TEST
#include "../main/link.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    link_arb_t a = { .owner = LINK_SRC_NONE, .until_ms = 0, .sticky = false };

    /* Nobody owns it: anyone may take it. */
    assert(link_arb_lapsed(&a, 0));
    assert(link_arb_grant(&a, LINK_SRC_RT, 1000, 300, false));
    assert(a.owner == LINK_SRC_RT);
    assert(!link_arb_lapsed(&a, 1000));
    assert(!link_arb_lapsed(&a, 1299));
    assert(link_arb_lapsed(&a, 1300));       /* the grant lapses exactly on its deadline */

    /* A live owner refuses anything below it, and refusing does not disturb the grant. */
    assert(!link_arb_grant(&a, LINK_SRC_CONSOLE, 1100, 0, true));
    assert(!link_arb_grant(&a, LINK_SRC_RECOVER, 1100, 0, true));
    assert(a.owner == LINK_SRC_RT);
    assert(a.until_ms == 1300);

    /* Equal rank refreshes: this is the 10 Hz stream holding its own grant open. */
    assert(link_arb_grant(&a, LINK_SRC_RT, 1200, 300, false));
    assert(a.until_ms == 1500);

    /* Higher rank pre-empts. */
    assert(link_arb_grant(&a, LINK_SRC_CALIB, 1250, 600, false));
    assert(a.owner == LINK_SRC_CALIB);
    assert(link_arb_grant(&a, LINK_SRC_SAFE, 1260, 0, true));
    assert(a.owner == LINK_SRC_SAFE);

    /* Sticky ownership never lapses on time. */
    assert(!link_arb_lapsed(&a, 1260));
    assert(!link_arb_lapsed(&a, 0xFFFFFFFFu));

    /* Release frees it, and only for the source that holds it. */
    link_arb_release(&a, LINK_SRC_RT);              /* not the owner — ignored */
    assert(a.owner == LINK_SRC_SAFE);
    link_arb_release(&a, LINK_SRC_SAFE);
    assert(a.owner == LINK_SRC_NONE);
    assert(link_arb_lapsed(&a, 1260));

    /* Once lapsed, the lowest source may take it. */
    a = (link_arb_t){ .owner = LINK_SRC_RT, .until_ms = 1300, .sticky = false };
    assert(link_arb_grant(&a, LINK_SRC_RECOVER, 1300, 0, true));
    assert(a.owner == LINK_SRC_RECOVER);

    /* Millisecond-counter rollover: a deadline just past UINT32_MAX still expires
       in order, the same way recovery_evict and watchdog_stale handle it. */
    a = (link_arb_t){ .owner = LINK_SRC_RT, .until_ms = 0xFFFFFF00u + 300,
                      .sticky = false };
    assert(!link_arb_lapsed(&a, 0xFFFFFF00u));      /* before the deadline */
    assert(link_arb_lapsed(&a, 0x00000100u));       /* wrapped past it */

    /* Names are for telemetry's "ctl" field and for logs. */
    assert(strcmp(link_src_name(LINK_SRC_NONE), "none") == 0);
    assert(strcmp(link_src_name(LINK_SRC_RT), "rt") == 0);
    assert(strcmp(link_src_name(LINK_SRC_SAFE), "safe") == 0);
    assert(strcmp(link_src_name((link_src_t)99), "?") == 0);

    printf("test_link: all passed\n");
    return 0;
}
```

- [ ] **Step 2: Run it to verify it fails**

```bash
cd ~/VSCode/esp32-p4-car/firmware/p4/test
cc -I../main -Wall -Wextra -Werror -std=c11 -o /tmp/test_link test_link.c -lm
```

Expected: `fatal error: '../main/link.h' file not found`.

- [ ] **Step 3: Write the header**

Create `firmware/p4/main/link.h`:

```c
#ifndef LINK_H
#define LINK_H

#include <stdint.h>
#include <stdbool.h>

/* Who may command the actuator.
 *
 * The numeric order IS the priority, ascending: a request is granted when its source
 * ranks at or above the current owner. Each neighbouring pair is a decision:
 *
 *   RT above RECOVER   — when the driver comes back, the next control frame takes the
 *                        wheels back immediately. If the retreat outranked it, control
 *                        would be refused while a frame was arriving.
 *   CONSOLE above RECOVER — a bench command is not silently killed by a retreat. The
 *                        retreat can only run after real /ws traffic armed the watchdog,
 *                        so this costs nothing in a console-only session.
 *   RT above CONSOLE   — a live pilot outranks a command typed minutes ago.
 *   CALIB above RT     — the wizard's spin pulse must not be overwritten ~100 ms later
 *                        by the app's own 10 Hz zero-stream.
 *   OTA, SAFE on top   — flashing and an explicit stop answer to nobody.
 */
typedef enum {
    LINK_SRC_NONE    = -1,
    LINK_SRC_RECOVER = 0,
    LINK_SRC_CONSOLE,
    LINK_SRC_RT,
    LINK_SRC_CALIB,
    LINK_SRC_OTA,
    LINK_SRC_SAFE,
} link_src_t;

/* Pure arbitration state. Owned by link.c; exposed here so it can be host-tested. */
typedef struct {
    link_src_t owner;
    uint32_t   until_ms;   /* when a non-sticky grant expires */
    bool       sticky;     /* ownership ends only on release, never on time */
} link_arb_t;

/* Pure: is the actuator free at `now`?
 * Unsigned subtraction cast to signed, so the 32-bit millisecond counter's rollover
 * is handled the same way watchdog_stale and recovery_evict handle it. */
static inline bool link_arb_lapsed(const link_arb_t *a, uint32_t now) {
    if (a->owner == LINK_SRC_NONE) return true;
    if (a->sticky) return false;
    return (int32_t)(now - a->until_ms) >= 0;
}

/* Pure: may `src` command the actuator at `now`? Records the grant on success.
 * A refused request leaves the current grant exactly as it was. */
static inline bool link_arb_grant(link_arb_t *a, link_src_t src, uint32_t now,
                                  uint32_t hold_ms, bool sticky) {
    if (!link_arb_lapsed(a, now) && src < a->owner) return false;
    a->owner    = src;
    a->until_ms = now + hold_ms;
    a->sticky   = sticky;
    return true;
}

/* Pure: give up ownership, but only if `src` is the one holding it — a late release
 * must not steal the actuator from whoever has since taken over. */
static inline void link_arb_release(link_arb_t *a, link_src_t src) {
    if (a->owner != src) return;
    a->owner    = LINK_SRC_NONE;
    a->until_ms = 0;
    a->sticky   = false;
}

/* Pure: the name telemetry reports in "ctl", and logs use. */
static inline const char *link_src_name(link_src_t s) {
    switch (s) {
        case LINK_SRC_NONE:    return "none";
        case LINK_SRC_RECOVER: return "recover";
        case LINK_SRC_CONSOLE: return "console";
        case LINK_SRC_RT:      return "rt";
        case LINK_SRC_CALIB:   return "calib";
        case LINK_SRC_OTA:     return "ota";
        case LINK_SRC_SAFE:    return "safe";
        default:               return "?";
    }
}

#ifndef LINK_HOST_TEST
#include "esp_err.h"

/* How long each source's grant holds without being refreshed. */
#define LINK_HOLD_RT_MS     300u   /* the watchdog deadline; the 10 Hz stream refreshes it */
#define LINK_HOLD_CALIB_MS  600u   /* one identification pulse */

/* Start the 50 Hz actuator task. The sole writer to the PCA9685 after this call. */
esp_err_t link_init(void);

/* Ask to set the eight duties. Returns false when a higher-priority source holds the
 * actuator, in which case nothing is written and the caller must not treat the command
 * as applied — the control watchdog is fed only on a true return. */
bool link_set(link_src_t src, const uint16_t duty[8], uint32_t hold_ms, bool sticky);

/* Give up ownership held by `src`. Harmless if `src` does not hold it. */
void link_release(link_src_t src);

/* Who owns the actuator right now, for telemetry and logs. */
link_src_t link_owner(void);

/* False once a PCA9685 write has failed and not yet succeeded again. */
bool link_bus_ok(void);
#endif /* LINK_HOST_TEST */

#endif /* LINK_H */
```

- [ ] **Step 4: Run the test to verify it passes**

```bash
cd ~/VSCode/esp32-p4-car/firmware/p4/test
cc -I../main -Wall -Wextra -Werror -std=c11 -o /tmp/test_link test_link.c -lm && /tmp/test_link
```

Expected: `test_link: all passed`.

- [ ] **Step 5: Wire it into the host Makefile**

In `firmware/p4/test/Makefile`, add `test_link` to the `all:` list, to the `run:` chain and to `clean:`, and add the rule in the same shape as the others:

```make
test_link: test_link.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)
```

- [ ] **Step 6: Run every host test**

```bash
cd ~/VSCode/esp32-p4-car && ./tools/test-all.sh 2>&1 | tail -6
```

Expected: `test_link: all passed` among the rest, ending in `== all green ==`.

- [ ] **Step 7: Commit**

```bash
cd ~/VSCode/esp32-p4-car
git add firmware/p4/main/link.h firmware/p4/test/test_link.c firmware/p4/test/Makefile
git commit -m "feat(fw): the actuator arbiter, as a pure state machine

Five things want to command the motors and nothing arbitrates between them:
ramp_set_target is a memcpy under a lock, so the last caller wins silently with
no record of the conflict. This is the state machine that replaces that.

The priority order is argued in the header rather than asserted, because every
neighbouring pair is a real decision. The one that fixes a live defect is CALIB
above RT: today the calibration wizard's spin pulse is overwritten about 100 ms
later by the app's own 10 Hz zero-stream.

Pure and host-tested, including rollover of the millisecond counter, the same
way ramp_step and watchdog_stale are.
"
```

---

### Task 2: Bound the I2C wait, and be able to zero the chip

`pca9685.c:107` passes `-1` as the I2C timeout, which is "wait forever". A slave holding SDA low — a loose cable, a glitched board, and there are three devices on this bus — freezes the sole actuator writer permanently. Separately, nothing can currently command the chip to zero without going through the ramp, which Task 3 needs at boot.

**Files:**
- Modify: `firmware/p4/main/pca9685.c`, `firmware/p4/main/pca9685.h`

**Interfaces:**
- Consumes: nothing.
- Produces: `esp_err_t pca9685_zero_all(void)` — full-off on all channels of both boards.

- [ ] **Step 1: Bound every I2C wait and retry once**

In `firmware/p4/main/pca9685.c`, under the existing `#define`s add:

```c
/* Never wait forever on the bus. The sole actuator writer runs here, and a slave
   holding SDA low would otherwise freeze the motors at their last duty until the
   battery is disconnected. Three devices share this bus, one of them an audio
   codec we do not drive. */
#define PCA_I2C_TIMEOUT_MS 50
```

Replace the three `-1` timeouts with `PCA_I2C_TIMEOUT_MS`:

```c
static esp_err_t pca9685_write_reg(int idx, uint8_t reg, uint8_t value) {
    uint8_t buf[2] = {reg, value};
    return i2c_master_transmit(s_dev[idx], buf, sizeof(buf), PCA_I2C_TIMEOUT_MS);
}

static esp_err_t pca9685_read_reg(int idx, uint8_t reg, uint8_t *value) {
    return i2c_master_transmit_receive(s_dev[idx], &reg, 1, value, 1, PCA_I2C_TIMEOUT_MS);
}
```

And at the end of `pca9685_set_pwm`, replace the single `return i2c_master_transmit(...)` with:

```c
    esp_err_t e = i2c_master_transmit(s_dev[idx], buf, sizeof(buf), PCA_I2C_TIMEOUT_MS);
    if (e != ESP_OK) {
        /* One retry: a single NACK on a shared bus is worth another try, and the
           caller treats a second failure as a bus fault rather than retrying forever. */
        e = i2c_master_transmit(s_dev[idx], buf, sizeof(buf), PCA_I2C_TIMEOUT_MS);
    }
    return e;
```

- [ ] **Step 2: Add `pca9685_zero_all`**

Append to `firmware/p4/main/pca9685.c`:

```c
esp_err_t pca9685_zero_all(void) {
    /* Full-off on every channel of both boards, including the eight this project
       does not drive. The chip's registers survive a P4 reset, so after a panic
       reboot or an OTA restart the outputs are whatever they were mid-drive. */
    esp_err_t first_err = ESP_OK;
    for (int i = 0; i < PCA_COUNT; i++) {
        for (uint8_t ch = 0; ch < BOARD_PCA_CH_PER_CHIP; ch++) {
            uint8_t base_reg = PCA9685_LED0_ON_L + 4 * ch;
            uint8_t buf[5] = { base_reg, 0x00, 0x00, 0x00, 0x10 };  /* OFF bit 12 set */
            esp_err_t e = i2c_master_transmit(s_dev[i], buf, sizeof(buf), PCA_I2C_TIMEOUT_MS);
            if (e != ESP_OK && first_err == ESP_OK) first_err = e;
        }
    }
    return first_err;
}
```

In `firmware/p4/main/pca9685.h`, declare it above the closing guard:

```c
// Drive every channel of every board fully off. Used at boot, because the chip's
// registers survive a P4 reset and the firmware's idea of "stopped" does not.
esp_err_t pca9685_zero_all(void);
```

- [ ] **Step 3: Build the firmware**

```bash
cd ~/VSCode/esp32-p4-car/firmware/p4
source ~/esp/esp-idf-v6.0.2/export.sh >/dev/null 2>&1
idf.py build 2>&1 | tail -4
```

Expected: `Project build complete.`

- [ ] **Step 4: Commit**

```bash
cd ~/VSCode/esp32-p4-car
git add firmware/p4/main/pca9685.c firmware/p4/main/pca9685.h
git commit -m "fix(fw): bound the I2C wait, and give the chip a way to be zeroed

The sole actuator writer waited forever on the bus. A slave holding SDA low —
a loose cable, a glitched board, and three devices share this bus — would have
frozen the motors at their last duty until the battery came off. Fifty
milliseconds and one retry instead.

pca9685_zero_all exists because the chip's registers survive a P4 reset. After
a panic reboot or the restart at the end of an OTA, the outputs are still
whatever they were mid-drive, and nothing in the firmware was able to say
otherwise. Task 3 calls it at boot.
"
```

---

### Task 3: `link.c` — one owner for the actuator, one writer for the chip

The 50 Hz task moves out of `ramp.c` and into `link.c`, which also holds the arbiter. `ramp` keeps the pure `ramp_step` and its NVS-backed setting; `ramp_set_target` ceases to exist, which is what makes "one commander" structural rather than a convention.

Two bugs are fixed by construction here. The shadow `s_current[ch]` is currently updated *before* the write succeeds, so one NACK leaves the firmware believing a channel is at zero while the motor keeps spinning, and `dirty` never rises again for it. And at boot `s_current == s_target == 0`, so `dirty` is false and **nothing at all is written** — the safety stop never reaches the chip.

**Files:**
- Create: `firmware/p4/main/link.c`
- Modify: `firmware/p4/main/ramp.c`, `firmware/p4/main/ramp.h`, `firmware/p4/main/CMakeLists.txt`

**Interfaces:**
- Consumes: `link_arb_*` from Task 1; `pca9685_set_pwm`, `pca9685_zero_all` from Task 2; `ramp_step`, `ramp_get_ms` from `ramp.h`.
- Produces: `link_init`, `link_set`, `link_release`, `link_owner`, `link_bus_ok` as declared in Task 1's header.

- [ ] **Step 1: Write `link.c`**

Create `firmware/p4/main/link.c`:

```c
#include "link.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "pca9685.h"
#include "ramp.h"

static const char *TAG = "link";

#define TICK_MS 20                 /* 50 Hz */
#define SHADOW_UNKNOWN 0xFFFFu     /* forces a real write on the first tick */

static SemaphoreHandle_t s_lock;   /* guards s_arb and s_target */
static link_arb_t        s_arb = { .owner = LINK_SRC_NONE, .until_ms = 0, .sticky = false };
static uint16_t          s_target[8];
static uint16_t          s_current[8];
static volatile bool     s_bus_ok = true;

static uint32_t now_ms(void) {
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

bool link_bus_ok(void) { return s_bus_ok; }

link_src_t link_owner(void) {
    if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) != pdTRUE) return LINK_SRC_NONE;
    link_src_t o = link_arb_lapsed(&s_arb, now_ms()) ? LINK_SRC_NONE : s_arb.owner;
    xSemaphoreGive(s_lock);
    return o;
}

bool link_set(link_src_t src, const uint16_t duty[8], uint32_t hold_ms, bool sticky) {
    if (!s_lock) return false;
    /* A short wait, not the 200 ms the old car_drive used: this lock is held only for
       a memcpy and a struct assignment, so anything longer means something is wrong,
       and a control frame is better dropped than delayed a fifth of a second. */
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) != pdTRUE) {
        ESP_LOGW(TAG, "%s: lock busy, command dropped", link_src_name(src));
        return false;
    }
    bool granted = link_arb_grant(&s_arb, src, now_ms(), hold_ms, sticky);
    if (granted) {
        memcpy(s_target, duty, sizeof(s_target));
    }
    link_src_t owner = s_arb.owner;
    xSemaphoreGive(s_lock);

    if (!granted) {
        /* Rate-limited: a refused source is usually refused at its own frame rate. */
        static uint32_t last_log;
        uint32_t t = now_ms();
        if ((uint32_t)(t - last_log) > 1000) {
            last_log = t;
            ESP_LOGW(TAG, "%s refused: %s holds the actuator",
                     link_src_name(src), link_src_name(owner));
        }
    }
    return granted;
}

void link_release(link_src_t src) {
    if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) != pdTRUE) return;
    if (s_arb.owner == src) {
        link_arb_release(&s_arb, src);
        memset(s_target, 0, sizeof(s_target));   /* nobody owns it → the safe target */
    }
    xSemaphoreGive(s_lock);
}

static void link_task(void *arg) {
    (void)arg;
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(TICK_MS));

        uint16_t tgt[8];
        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(TICK_MS)) != pdTRUE) continue;
        /* An expired grant means nobody is driving: fall to zero rather than holding
           the last command, which is what "ownership lapses" has to mean physically. */
        if (link_arb_lapsed(&s_arb, now_ms())) {
            s_arb.owner = LINK_SRC_NONE;
            memset(s_target, 0, sizeof(s_target));
        }
        memcpy(tgt, s_target, sizeof(tgt));
        xSemaphoreGive(s_lock);

        uint16_t up = ramp_max_up_per_tick(ramp_get_ms(), TICK_MS);
        bool all_ok = true;
        for (uint8_t ch = 0; ch < 8; ch++) {
            uint16_t next = ramp_step(s_current[ch], tgt[ch], up);
            if (next == s_current[ch]) continue;
            esp_err_t e = pca9685_set_pwm(ch, next);
            if (e == ESP_OK) {
                s_current[ch] = next;          /* shadow follows the chip, not our intent */
            } else {
                /* Deliberately leave s_current alone. It still differs from the target,
                   so the next tick retries — where updating it first would have left the
                   firmware believing a spinning motor was stopped, forever. */
                all_ok = false;
                ESP_LOGE(TAG, "ch%u write failed: %s", ch, esp_err_to_name(e));
            }
        }
        if (all_ok) s_bus_ok = true;
        else        s_bus_ok = false;
    }
}

esp_err_t link_init(void) {
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    /* The chip's registers survive a P4 reset, so "we booted" is not "the motors are
       off". Say so to the hardware before anything else can command it. */
    esp_err_t e = pca9685_zero_all();
    if (e != ESP_OK) {
        s_bus_ok = false;
        ESP_LOGE(TAG, "could not zero the PCA9685 at boot: %s", esp_err_to_name(e));
    }
    for (int ch = 0; ch < 8; ch++) s_current[ch] = SHADOW_UNKNOWN;
    memset(s_target, 0, sizeof(s_target));

    return xTaskCreate(link_task, "link", 3072, NULL, 5, NULL) == pdPASS ? ESP_OK : ESP_FAIL;
}
```

- [ ] **Step 2: Shrink `ramp` to the setting and the pure step**

In `firmware/p4/main/ramp.h`, move the per-tick rise calculation into the pure section so `link.c` can use it, and delete the task-side API. Replace the whole file with:

```c
#ifndef RAMP_H
#define RAMP_H
#include <stdint.h>

// Pure slew-rate step: rise is limited to max_up per call, fall is instant (safe stop).
// Zero ESP-IDF deps — host-tested.
static inline uint16_t ramp_step(uint16_t current, uint16_t target, uint16_t max_up) {
    if (target <= current) return target;                 // fall (or equal): instant
    uint32_t next = (uint32_t)current + max_up;           // rise: bounded
    return next > target ? target : (uint16_t)next;
}

// Pure: how much a channel may rise in one tick, given the configured 0→full time.
// ramp_ms shorter than a tick (including 0, "off") means no limit at all.
static inline uint16_t ramp_max_up_per_tick(uint16_t ramp_ms, uint16_t tick_ms) {
    if (ramp_ms < tick_ms) return 4095;
    uint16_t step = (uint16_t)(4095u * tick_ms / ramp_ms);
    return step ? step : 1;
}

#ifndef RAMP_HOST_TEST
#include "esp_err.h"
// Load ramp_ms from NVS (default 300). No task: the actuator task lives in link.c.
esp_err_t ramp_init(void);
// Acceleration time 0→full in ms (0 = ramp off / instant). Clamped to 0..2000.
void ramp_set_ms(uint16_t ms);
uint16_t ramp_get_ms(void);
// Persist the current ramp_ms as a JSON string in NVS.
void ramp_save(void);
#endif
#endif // RAMP_H
```

In `firmware/p4/main/ramp.c`, delete `TICK_MS`, `max_up_per_tick`, `ramp_task`, `ramp_set_target`, `s_target`, `s_current`, and the `pca9685.h` and `freertos/task.h` includes. `ramp_init` keeps only the NVS load and returns `ESP_OK`:

```c
esp_err_t ramp_init(void) {
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    char buf[32];
    if (cfg_json_load("ramp", buf, sizeof(buf))) {
        cJSON *j = cJSON_Parse(buf);
        int v;
        if (cfg_json_int(j, "ramp_ms", &v) && v >= 0 && v <= RAMP_MS_MAX) {
            s_ramp_ms = (uint16_t)v;
        }
        cJSON_Delete(j);
    }
    ESP_LOGI(TAG, "ramp_ms = %u (boot)", s_ramp_ms);
    return ESP_OK;
}
```

- [ ] **Step 3: Extend the ramp host test to cover the moved function**

In `firmware/p4/test/test_ramp.c`, before the `printf`, add:

```c
    // ramp_max_up_per_tick: a full-scale rise spread over ramp_ms
    assert(ramp_max_up_per_tick(300, 20) == 4095u * 20 / 300);   // 273
    assert(ramp_max_up_per_tick(2000, 20) == 40);
    // ramp off, or a ramp shorter than one tick: no limit
    assert(ramp_max_up_per_tick(0, 20) == 4095);
    assert(ramp_max_up_per_tick(19, 20) == 4095);
    assert(ramp_max_up_per_tick(20, 20) == 4095);                // exactly one tick
    // never zero: a very long ramp still moves
    assert(ramp_max_up_per_tick(65535, 20) >= 1);
```

- [ ] **Step 4: Register `link.c` with the build**

In `firmware/p4/main/CMakeLists.txt`, add `"link.c"` to the `SRCS` list.

- [ ] **Step 5: Run the host tests**

```bash
cd ~/VSCode/esp32-p4-car && ./tools/test-all.sh 2>&1 | tail -6
```

Expected: `test_ramp: all passed` and `== all green ==`. The firmware will not build yet — `car.c` still calls `ramp_set_target`, and Task 4 fixes that.

- [ ] **Step 6: Commit**

```bash
cd ~/VSCode/esp32-p4-car
git add firmware/p4/main/link.c firmware/p4/main/ramp.c firmware/p4/main/ramp.h \
        firmware/p4/main/CMakeLists.txt firmware/p4/test/test_ramp.c
git commit -m "feat(fw): link.c owns the actuator, and ramp_set_target stops existing

The 50 Hz task moves out of ramp.c and in beside the arbiter, so one module owns
the actuator end to end. ramp keeps the pure step and its NVS-backed setting.
Removing ramp_set_target is the point: one commander becomes structural rather
than a convention nobody could enforce.

Two bugs go with it. The shadow was updated before the write succeeded, so a
single NACK left the firmware believing a channel was at zero while the motor
kept spinning — and since the shadow then matched the target, it never retried.
It now follows the chip, not our intent.

And at boot s_current == s_target == 0, so nothing was written at all: the
safety stop never reached a chip whose registers had survived the reset. Boot
now zeroes the hardware and starts the shadow at a value no duty can equal.
"
```

---

### Task 4: `car.c` — a calibration read that cannot block, and a stop that cannot be dropped

`car_drive` takes a mutex with a 200 ms timeout and, on timeout, returns having commanded **nothing** — so the watchdog's emergency stop can silently vanish. The lock exists only to keep a calibration write from tearing a read. An immutable config published by pointer swap removes the read side of the lock entirely, and with it both the stall and the failure mode.

**Files:**
- Modify: `firmware/p4/main/car.c`, `firmware/p4/main/car.h`

**Interfaces:**
- Consumes: `link_set`, `link_release`, `link_src_t` from Tasks 1 and 3.
- Produces: `bool car_drive(link_src_t src, float throttle, float yaw)`; `void car_stop(link_src_t src)`; `bool car_spin_pair(uint8_t pair, bool forward)`; the rest of `car.h` unchanged.

- [ ] **Step 1: Rewrite the state and the hot path**

In `firmware/p4/main/car.c`, replace the `g_cfg` / `g_lock` / `g_trim_pct` block and `car_drive` with:

```c
#include <stdatomic.h>
#include "link.h"

/* The calibration is immutable once published. Writers fill the spare buffer and swap
   the pointer; readers load it and never block. That matters more than it sounds: the
   old code took a mutex with a 200 ms timeout on the control path and, on timeout,
   returned having commanded nothing — so an emergency stop could silently vanish. */
static motors_config_t s_cfg_buf[2] = {
    [0] = {
        .wheels = {
            [POS_FL] = { .channel_pair = 0, .sign = 1 },
            [POS_FR] = { .channel_pair = 1, .sign = 1 },
            [POS_RL] = { .channel_pair = 2, .sign = 1 },
            [POS_RR] = { .channel_pair = 3, .sign = 1 },
        },
        .deadzone = 0.05f,
    },
};
static _Atomic(const motors_config_t *) s_cfg = &s_cfg_buf[0];
static int s_cfg_next = 1;          /* only touched by config writers, which are serialised
                                       by the single httpd task */
static _Atomic int s_trim_pct = 0;  /* [-30..30] */

static float clamp_unit(float v) {
    if (v > 1.0f) return 1.0f;
    if (v < -1.0f) return -1.0f;
    return v;
}

static uint32_t hold_for(link_src_t src) {
    switch (src) {
        case LINK_SRC_RT:    return LINK_HOLD_RT_MS;
        case LINK_SRC_CALIB: return LINK_HOLD_CALIB_MS;
        default:             return 0;   /* sticky sources ignore this */
    }
}

static bool sticky_for(link_src_t src) {
    /* Anything that is not a stream holds until it says otherwise. A console command
       runs until the next one, which is the documented bench behaviour. */
    return src != LINK_SRC_RT && src != LINK_SRC_CALIB;
}

bool car_drive(link_src_t src, float throttle, float yaw) {
    throttle = clamp_unit(throttle);
    yaw = clamp_unit(yaw);
    side_speeds_t s = mixer_mix(throttle, yaw);

    trim_apply(&s.left, &s.right, (float)atomic_load(&s_trim_pct) / 100.0f);
    const motors_config_t *cfg = atomic_load(&s_cfg);
    motor_outputs_t out = motors_plan(s.left, s.right, cfg);

    bool applied = link_set(src, out.duty, hold_for(src), sticky_for(src));
    ESP_LOGD(TAG, "drive[%s] t=%.2f y=%.2f -> L=%.2f R=%.2f %s",
             link_src_name(src), throttle, yaw, s.left, s.right,
             applied ? "applied" : "REFUSED");
    return applied;
}

void car_stop(link_src_t src) {
    car_drive(src, 0.0f, 0.0f);
}
```

Replace `car_set_calibration`, `car_set_trim` and `car_get_trim` with:

```c
void car_set_calibration(const motors_config_t *cfg) {
    /* Publish a new immutable copy. Readers on the control path see either the old
       pointer or the new one, never a half-written struct. */
    s_cfg_buf[s_cfg_next] = *cfg;
    atomic_store(&s_cfg, &s_cfg_buf[s_cfg_next]);
    s_cfg_next ^= 1;
}

void car_set_trim(int8_t pct) {
    if (pct > 30) pct = 30;
    if (pct < -30) pct = -30;
    atomic_store(&s_trim_pct, pct);
}

int8_t car_get_trim(void) {
    return (int8_t)atomic_load(&s_trim_pct);
}
```

`car_spin_pair` gains a return value and a source:

```c
bool car_spin_pair(uint8_t pair, bool forward) {
    if (pair > 3) return false;
    motor_outputs_t out = { .duty = {0} };
    const uint16_t duty = 1600;  /* ~40% for identification */
    out.duty[pair * 2]     = forward ? duty : 0;
    out.duty[pair * 2 + 1] = forward ? 0 : duty;
    return link_set(LINK_SRC_CALIB, out.duty, LINK_HOLD_CALIB_MS, false);
}
```

And in `car_init`, delete the mutex creation and its `ESP_ERROR_CHECK`, and change the final safety stop to name its source:

```c
    car_stop(LINK_SRC_SAFE);
    link_release(LINK_SRC_SAFE);   /* boot is over; leave the actuator free */
```

Keep the NVS calibration load and the trim load exactly as they are, but route the loaded values through `car_set_calibration` and `car_set_trim` rather than assigning the statics directly.

- [ ] **Step 2: Update `car.h`**

```c
#include "link.h"
...
// Command the car. Returns false when a higher-priority source holds the actuator,
// in which case nothing was applied and the caller must not treat it as a live frame.
bool car_drive(link_src_t src, float throttle, float yaw);
void car_stop(link_src_t src);
bool car_spin_pair(uint8_t pair, bool forward);
```

- [ ] **Step 3: Build — it will fail, and the failures are the list of callers to fix**

```bash
cd ~/VSCode/esp32-p4-car/firmware/p4
source ~/esp/esp-idf-v6.0.2/export.sh >/dev/null 2>&1
idf.py build 2>&1 | grep -E "error:" | head -20
```

Expected: errors in `ws_control.c`, `main.c`, `recovery.c`, `ota_api.c`, `calib_api.c` — every caller of `car_drive`/`car_stop`. That list is Task 5's work; do not fix them here.

- [ ] **Step 4: Commit**

```bash
cd ~/VSCode/esp32-p4-car
git add firmware/p4/main/car.c firmware/p4/main/car.h
git commit -m "fix(fw): a calibration read that cannot block, a stop that cannot vanish

car_drive took a mutex with a 200 ms timeout on the control path and, on
timeout, returned having commanded nothing. The watchdog's emergency stop went
through that path, so it could silently do nothing at all — and since car_drive
also held that lock across ramp_set_target's own 200 ms wait, 400 ms of
contention was reachable.

The lock only ever protected a calibration write from tearing a read. The config
is now immutable and published by pointer swap: readers load a pointer and never
block, writers fill the spare buffer. Trim is a plain atomic.

car_drive also gains a source and a return value. Callers are broken by this
commit on purpose — the compiler's list of them is exactly the set that has to
declare what it is."
```

---

### Task 5: Every producer names itself

**Files:**
- Modify: `firmware/p4/main/ws_control.c`, `firmware/p4/main/main.c`, `firmware/p4/main/recovery.c`, `firmware/p4/main/ota_api.c`, `firmware/p4/main/calib_api.c`, `firmware/p4/main/watchdog.c`, `firmware/p4/main/telemetry.c`, `firmware/p4/main/telemetry.h`

**Interfaces:**
- Consumes: `car_drive(src, ...)`, `car_stop(src)`, `link_release`, `link_owner`, `link_bus_ok`, `link_src_name`.
- Produces: telemetry gains `ctl` (string) and `bus_ok` (bool).

- [ ] **Step 1: The control path feeds the watchdog only on a real apply**

In `firmware/p4/main/ws_control.c`, the handler currently reads:

```c
    if (control_parse_json((const char *)buf, &t, &y) == 0) {
        s_frames++;
        watchdog_feed();
        recovery_note_command(t, y);
        car_drive(t, y);
    } else {
```

Replace with:

```c
    if (control_parse_json((const char *)buf, &t, &y) == 0) {
        s_frames++;
        /* Feed the watchdog only when the command actually reached the actuator.
           Feeding first meant the one mechanism that could notice the actuator had
           stopped responding was fed by the frames that failed to reach it. */
        if (car_drive(LINK_SRC_RT, t, y)) {
            watchdog_feed();
            recovery_note_command(t, y);
        }
    } else {
```

- [ ] **Step 2: The console names itself**

In `firmware/p4/main/main.c`, add `#include "link.h"` and change the REPL's call:

```c
        if (parse_mix(line, &t, &y) == 0) {
            if (!car_drive(LINK_SRC_CONSOLE, t, y)) {
                ESP_LOGW(TAG, "refused: %s holds the actuator", link_src_name(link_owner()));
            }
        } else {
```

- [ ] **Step 3: Recovery takes and releases the actuator**

In `firmware/p4/main/recovery.c`, add `#include "link.h"`. In `retreat_task`, replace the three `car_stop()` calls with `car_stop(LINK_SRC_RECOVER)` and `car_drive(rt, ry)` with `car_drive(LINK_SRC_RECOVER, rt, ry)`. At the end of the retreat — both the aborted and the exhausted branch — release:

```c
        if (aborted) {
            ESP_LOGI(TAG, "link returned — handing control back");
        } else {
            car_stop(LINK_SRC_RECOVER);
            ESP_LOGI(TAG, "retrace exhausted — stopped");
        }
        link_release(LINK_SRC_RECOVER);
```

In `recovery_on_link_lost`, the disabled branch becomes `car_stop(LINK_SRC_RECOVER); link_release(LINK_SRC_RECOVER); return;`.

- [ ] **Step 4: The watchdog revokes the dead stream before handing over**

In `firmware/p4/main/watchdog.c`, add `#include "link.h"` and in `wdt_cb`, before `recovery_on_link_lost()`:

```c
        /* The stream is gone. Revoke its grant explicitly rather than waiting for it
           to lapse at the same instant this fires, so recovery is not refused by a
           grant that is technically still alive. */
        link_release(LINK_SRC_RT);
        recovery_on_link_lost();
```

- [ ] **Step 5: OTA forces safe and holds it**

In `firmware/p4/main/ota_api.c`, replace `car_stop();` at the top of `ota_post` with:

```c
    car_stop(LINK_SRC_OTA);   /* sticky: nothing may command the motors during a flash */
```

- [ ] **Step 6: Calibration reports a refusal instead of pretending**

In `firmware/p4/main/calib_api.c`, replace the spin body's tail:

```c
    ESP_LOGI(TAG, "spin pair %d %s", pair, dir ? "fwd" : "rev");
    car_spin_pair((uint8_t)pair, dir != 0);
    vTaskDelay(pdMS_TO_TICKS(600));
    car_stop();
    return httpd_resp_sendstr(req, "ok");
```

with:

```c
    ESP_LOGI(TAG, "spin pair %d %s", pair, dir ? "fwd" : "rev");
    if (!car_spin_pair((uint8_t)pair, dir != 0)) {
        return httpd_resp_send_err(req, HTTPD_409_CONFLICT, "actuator busy");
    }
    /* The grant lapses on its own after LINK_HOLD_CALIB_MS, so the pulse ends whether
       or not this handler is still here. The delay is only so the reply lands after
       the wheel has stopped, which is what the wizard's next step assumes. */
    vTaskDelay(pdMS_TO_TICKS(LINK_HOLD_CALIB_MS));
    link_release(LINK_SRC_CALIB);
    return httpd_resp_sendstr(req, "ok");
```

Add `#include "link.h"`.

- [ ] **Step 7: Telemetry reports the owner and the bus**

In `firmware/p4/main/telemetry.h`, add to `telemetry_t`:

```c
    const char *ctl;      /* which source owns the actuator */
    bool        bus_ok;   /* false once a PCA9685 write has failed and not recovered */
```

In `telemetry.c`'s `telemetry_gather`, set them:

```c
    out->ctl        = link_src_name(link_owner());
    out->bus_ok     = link_bus_ok();
```

and in `telemetry_fields` add them to the format string, keeping the existing fields and order:

```c
             ",\"ctl\":\"%s\",\"bus_ok\":%s"
```

Extend the `fields` buffer in `telemetry_json` and in `status_api.c` from 160 to 224 bytes to hold them.

- [ ] **Step 8: Update the telemetry host test**

`firmware/p4/test/test_telemetry.c` tests `telemetry_fields`. Add the two fields to whatever fixture it builds and assert they appear:

```c
    assert(strstr(buf, "\"ctl\":\"rt\"") != NULL);
    assert(strstr(buf, "\"bus_ok\":true") != NULL);
```

- [ ] **Step 9: Build and run everything**

```bash
cd ~/VSCode/esp32-p4-car/firmware/p4
source ~/esp/esp-idf-v6.0.2/export.sh >/dev/null 2>&1
idf.py build 2>&1 | tail -4
cd ~/VSCode/esp32-p4-car && ./tools/test-all.sh 2>&1 | tail -4
```

Expected: `Project build complete.` and `== all green ==`.

- [ ] **Step 10: Commit**

```bash
cd ~/VSCode/esp32-p4-car
git add firmware/p4/main firmware/p4/test
git commit -m "feat(fw): every producer names its source, and telemetry reports the owner

The compiler's error list from the previous commit was the set of things that
command the motors: the control socket, the console REPL, the retreat task, the
OTA handler and the calibration wizard. Each now says which it is, and each gets
an answer.

Three behaviours change. The watchdog is fed only when a frame actually reached
the actuator — feeding first meant the one mechanism that could notice the
actuator had stopped responding was fed by the frames that failed to reach it.
The watchdog revokes the dead stream's grant before handing over, rather than
racing its expiry. And /calib/spin answers 409 when something outranks it
instead of returning ok for a wheel that never turned.

Telemetry gains ctl and bus_ok. Both are additive, so an older app ignores them."
```

---

### Task 6: A motor bus that is dead should not take the network with it

`app_main` starts with `ESP_ERROR_CHECK(pca9685_bus_init(...))` and `ESP_ERROR_CHECK(pca9685_init(...))`. A dead I2C bus therefore means a boot loop with no Wi-Fi, no API and no OTA — recoverable only over USB. That is exactly what the bench hit while the PCA9685 boards were still unwired, and `docs/bringup.md` records it as a known trap rather than a bug.

**Files:**
- Modify: `firmware/p4/main/main.c`

- [ ] **Step 1: Make the motor bus a soft failure**

Replace the first two lines of `app_main` with:

```c
    /* A dead motor bus must not take the radio with it. Booting into the network
       with bus_ok=false is diagnosable and OTA-recoverable; a boot loop needs a
       USB cable and tells you nothing. */
    bool motors_ok = pca9685_bus_init(BOARD_I2C_SDA, BOARD_I2C_SCL, BOARD_I2C_HZ) == ESP_OK
                  && pca9685_init(BOARD_PWM_HZ) == ESP_OK;
    if (!motors_ok) {
        ESP_LOGE(TAG, "motor bus did not come up — the car will not drive, "
                      "but the network and OTA will. Check I2C wiring and power.");
    }
```

`link_init` already reports a failed `pca9685_zero_all()` through `bus_ok`, so no extra plumbing is needed.

- [ ] **Step 2: Reorder the boot so the arbiter exists before anything can command it**

`app_main`'s init sequence becomes, in order: motor bus (soft) → NVS → `ramp_init` → `link_init` → `car_init` → `wheel_init` → `dims_init` → `wifi_ap_start` → `http_server_start` → the `*_api_start` calls → `telemetry_start` → `recovery_init` → `watchdog_init` → console.

`ramp_init` must precede `link_init` because the actuator task reads `ramp_get_ms()` on its first tick.

- [ ] **Step 3: Build and verify the binary still fits**

```bash
cd ~/VSCode/esp32-p4-car/firmware/p4
source ~/esp/esp-idf-v6.0.2/export.sh >/dev/null 2>&1
idf.py build 2>&1 | grep -E "binary size|Project build complete"
```

Expected: `Project build complete.` and a size still far under the 0x400000 partition. The baseline before this plan was 0xbaf00.

- [ ] **Step 4: Commit**

```bash
cd ~/VSCode/esp32-p4-car
git add firmware/p4/main/main.c
git commit -m "fix(fw): a dead motor bus no longer takes the radio with it

app_main aborted on the I2C bus, so a car with an unplugged PCA9685 boot-looped
with no Wi-Fi, no API and no OTA — recoverable only over USB. The bench hit
exactly this while the boards were unwired, and bringup.md recorded it as a
trap rather than a bug.

It now boots into the network and says bus_ok: false. That is diagnosable from
the app and fixable over the air; a boot loop is neither.

The init order also moves link_init ahead of everything that can command the
actuator, so no producer can reach a target buffer that does not exist yet."
```

---

### Task 7: Document the two new telemetry fields

**Files:**
- Modify: `docs/protocol.md`

- [ ] **Step 1: Extend the telemetry section**

In `docs/protocol.md`, under `## Telemetry`, update the sample frame to include the two fields and add to the prose beneath it:

```markdown
`ctl` names the source that currently owns the actuator — `rt`, `console`, `calib`,
`recover`, `ota`, `safe`, or `none`. It is how a client can tell "the car is ignoring me
because something outranks me" from "the car is not hearing me". A car that is retreating
under its own command reports `recover`, which is the only way to show that honestly.

`bus_ok` is false once a write to the motor driver has failed and not since succeeded. A
car with `bus_ok: false` is reachable, updatable and undriveable, which is a state worth
distinguishing from being offline.
```

- [ ] **Step 2: Confirm the generated region is untouched**

```bash
cd ~/VSCode/esp32-p4-car && bash tools/check_contract.sh
```

Expected: `contract: no drift`. The telemetry prose is hand-written; only the endpoint table is generated.

- [ ] **Step 3: Commit**

```bash
cd ~/VSCode/esp32-p4-car
git add docs/protocol.md
git commit -m "docs: ctl and bus_ok in the telemetry frame"
```

---

## Self-Review

**Spec coverage.** This plan implements the spec's `link.c` arbiter, all four actuator-safety fixes, the soft boot, the watchdog-feed reorder, the non-blocking `/calib/spin` grant, and the `ctl` telemetry field. It does **not** implement: the UDP channel, deleting `ws_control.c`, the explicit `bye`, the generic `cfg_api`, or the cheap-telemetry work (cached `calibrated`, async RSSI) — those are B2 and B3.

**Deliberate deviation from the spec.** The spec said "`ramp_set_target` becomes private to `link.c`". Keeping it public and documenting that only `link.c` may call it would be a convention, not a structure. Moving the task into `link.c` and deleting the symbol makes it enforced by the compiler, which is what the spec was reaching for.

**Placeholders.** None.

**Type consistency.** `link_src_t`, `link_arb_t`, `link_arb_lapsed/grant/release`, `link_src_name`, `link_init/set/release/owner/bus_ok`, `LINK_HOLD_RT_MS`, `LINK_HOLD_CALIB_MS`, `ramp_max_up_per_tick`, `car_drive(src,...)`, `car_stop(src)`, `car_spin_pair` returning `bool` — each is spelled identically in the task that defines it and every task that uses it.

**Known risk.** Task 4 deliberately leaves the tree unbuildable between commits 4 and 5. That is a bisect hazard, and it is worth it: the compiler's error list is the authoritative set of actuator producers, and discovering it by grep instead would risk missing one.

---

## Review outcomes

An adversarial review of the finished branch found seven defects. Five were introduced by this
plan; two were overstatements in its own commit messages.

| | Finding | Outcome |
|---|---|---|
| S1 | Putting CALIB above RT made every calibration spin refuse 6 control frames, so the watchdog tripped mid-spin and launched a retreat that ignored its own refusals and fired the moment CALIB lapsed | **Fixed.** The watchdog feeds on a parsed frame (link liveness); the breadcrumb stays gated on the grant; a refused retreat step aborts the retreat |
| S2 | `pca9685_init` ends with MODE1\|RESTART, which restores the pre-reset duty, and the zeroing sat behind an `nvs_flash_init` that can erase flash | **Fixed.** Zero immediately after init. `pca9685_zero_all` also covered 4 channels per chip instead of 16 |
| S3 | A NULL device handle made IDF log per call: ~1200 lines/s into a 115200 console, defeating the point of booting with a dead bus | **Fixed.** Refuse a NULL handle; rate-limit the remaining failures |
| S4 | `car_stop` and `link_release` returned void, so every emergency stop discarded whether it happened | **Fixed.** Both return a result; the watchdog and OTA act on it |
| S5 | `link_owner` took a 20 ms mutex from an esp_timer callback and reported `none` on timeout | **Fixed.** The owner is published under the lock and read without it |
| S6 | `bus_ok` flipped true on a tick that attempted no write, so a dead bus on a stationary car read healthy | **Fixed.** Only a tick that wrote may clear it |
| S7 | `LINK_SRC_CONSOLE` was the only sticky source with no release path | **Fixed.** `mix 0 0` releases |

**Deliberately not fixed here, and why:**

- **No bus-fault escalation.** Bounding the I2C wait stops the *task* hanging; it does not stop the
  *motors*, which hold their last duty on a wedged bus while `link_task` retries forever. An
  `i2c_master_bus_reset()` after N consecutive failures is the standard recovery and is worth
  doing — but it is new behaviour with its own failure modes, and inventing it at review time is
  how unreviewed code ships. The commit message for the timeout fix was corrected not to claim
  more than it delivers. **Carry to B2.**
- **The watchdog still runs on `Tmr Svc` at priority 1** and takes a 20 ms mutex there. B3 moves the
  watchdog into the `rt_link` loop, which removes the callback context entirely; fixing it twice
  would be churn.
- **The calibration double-buffer can be reused under a live reader.** Bounded at roughly 1e-4 per
  double-save, and provably not shoot-through: `motors_plan` writes both channels of a pair on
  every branch, so a torn table costs one wheel one 20 ms tick. Documented in place rather than
  traded for a permanent allocation.

**Pre-existing, confirmed by the review, already scheduled:** the `ws_fps` statics are
read-modify-written from two tasks; `calibration_load` does an NVS read from a 5 Hz timer callback;
`/calib/spin` blocks the single httpd task for 600 ms. All three are B2/B3 work.
