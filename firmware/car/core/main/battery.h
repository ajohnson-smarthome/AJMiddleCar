#ifndef BATTERY_H
#define BATTERY_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "contract.h"       /* the BATTERY_STATE_* vocabulary — the schema's, not ours */
#include "battery_pack.h"   /* BATTERY_LOW_PCT, BATTERY_LOW_CLEAR_PCT */

// The pack monitor as the rest of the firmware sees it (car/battery-monitor): a task that
// reads the chip behind power_monitor.h every BATTERY_PERIOD_MS, runs the reading through
// battery_soc, and publishes one snapshot for telemetry_gather. Nothing here touches the
// actuator — the monitor measures, and `low` is a word for the pult, not a limit.

// Between reads. "Not less than five times a second" is the spec's floor; the chip's own
// conversion cycle at the averaging ina260.c asks for is ~35 ms, so every read is fresh.
#define BATTERY_PERIOD_MS 200

// Failed reads in a row before the word says `absent`. One NACK on a bus shared with the
// PWM boards and the camera is a NACK, not a missing module; three in a row — 600 ms — is.
#define BATTERY_ABSENT_AFTER 3

/* ---- the verdict, as arithmetic ------------------------------------------------------
   Pure, pinned by test_battery.c: how many reads in a row failed, and whether the
   remainder says `low`. The task feeds one call per read; the word comes out here. */
typedef struct {
    unsigned failed;   // reads in a row that failed; saturates at BATTERY_ABSENT_AFTER
    bool     low;      // the hysteresis's memory: under BATTERY_LOW_PCT once, and not yet
                       // back above BATTERY_LOW_CLEAR_PCT
} battery_rule_t;

// Nothing has been measured yet, so the word starts at `absent` — not `ok` with no numbers.
// The first good read flips it, which is also the moment the remainder starts.
static inline void battery_rule_init(battery_rule_t *r) {
    memset(r, 0, sizeof(*r));
    r->failed = BATTERY_ABSENT_AFTER;
}

static inline bool battery_rule_absent(const battery_rule_t *r) { return r->failed >= BATTERY_ABSENT_AFTER; }

// A read that failed. Returns true on the call that made it `absent` — the transition to
// log once, not on every retry after it. A pack behind a module that stopped answering is
// not known to be the pack that was `low`, so that memory goes with the numbers.
static inline bool battery_rule_failed(battery_rule_t *r) {
    if (battery_rule_absent(r)) return false;
    r->failed++;
    if (!battery_rule_absent(r)) return false;
    r->low = false;
    return true;
}

// A read that succeeded, with the remainder `soc` as battery_soc_step gave it: 0..100, or -1
// while the start window has not closed. `low` is a statement about a number — in at
// BATTERY_LOW_PCT, out at BATTERY_LOW_CLEAR_PCT, and an undetermined remainder is `ok`.
// The caller checks battery_rule_absent BEFORE this call: a read that brings the monitor
// back is the one after which the remainder starts over (battery_soc_reset).
static inline void battery_rule_read(battery_rule_t *r, int soc) {
    r->failed = 0;
    if (soc < 0)      r->low = false;
    else if (r->low)  r->low = soc < BATTERY_LOW_CLEAR_PCT;
    else              r->low = soc <= BATTERY_LOW_PCT;
}

static inline const char *battery_rule_word(const battery_rule_t *r) {
    if (battery_rule_absent(r)) return BATTERY_STATE_ABSENT;
    return r->low ? BATTERY_STATE_LOW : BATTERY_STATE_OK;
}

/* ---- the snapshot ------------------------------------------------------------------ */
// What telemetry_gather reads: the last verdict and the last reading, copied together
// under a critical section, so a frame never carries `ok` with one read's voltage and
// another's current. The wire's `null` is telemetry's business: here `present` says whether
// the numbers are there at all, and `soc` is -1 while the remainder is undetermined.
typedef struct {
    const char *state;   // one of the BATTERY_STATE_* words
    bool        present; // false is `absent`: mv/ma/mw/soc carry nothing
    int32_t     mv;      // pack voltage, mV
    int32_t     ma;      // current, mA, discharge positive
    int32_t     mw;      // power, mW, as the monitor computes it
    int         soc;     // remaining charge 0..100, or -1 while not yet determined
} battery_snapshot_t;

#ifndef BATTERY_HOST_TEST
#include "esp_err.h"

// Detect the monitor on the shared bus (i2c_bus.h — after i2c_bus_init) and start the task.
// Not a boot failure in any case the monitor can cause: no chip, a chip with a foreign id,
// a bus that never came up — the snapshot says `absent` and the car drives. ESP_FAIL only
// when the task itself could not be created. Never touches motors_ok: motors.bus is about
// the PWM boards.
esp_err_t battery_init(void);

// The latest snapshot, from any task. `absent` with -1 and zeros before battery_init and
// on a car without a monitor.
void battery_snapshot(battery_snapshot_t *out);
#endif /* BATTERY_HOST_TEST */

#endif // BATTERY_H
