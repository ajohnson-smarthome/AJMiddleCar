/* POST /calibration/spin after its body has passed validation: what the handler does to
 * the world, in order, over a recorder — the test_rt_glue idiom. The recorder is the world:
 * every effect appends its name, so "nothing was touched" is a log of length one.
 *
 * AJM-100: a car whose PCA9685 bus was down answered 200 to a pulse — the grant was given,
 * the target set, every write skipped by the actuator (link.c gates them on
 * pca9685_ready()), and 600 ms later the handler released and said ok. The wizard took
 * the 200 as "the wheel turned" and let the user pick one, four times, into a table the
 * car accepts. The spec's word for a pulse that cannot turn a wheel is 409 busy. */
#include "../main/calib_spin.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    char    log[8][16];
    int     n;
    bool    bus_ok;      /* what link_bus_ok() would say */
    bool    granted;     /* what car_spin_pair() would say */
    uint8_t pair;        /* what car_spin_pair() was asked for */
    bool    forward;
} rec_t;

static void note(rec_t *r, const char *what) {
    snprintf(r->log[r->n], sizeof(r->log[0]), "%s", what);
    if (r->n < 7) r->n++;
}
static bool fx_bus_ok(void *c) { note(c, "bus?"); return ((rec_t *)c)->bus_ok; }
static bool fx_spin(void *c, uint8_t pair, bool forward) {
    rec_t *r = c;
    note(r, "spin");
    r->pair = pair;
    r->forward = forward;
    return r->granted;
}
static void fx_hold(void *c)    { note(c, "hold"); }
static void fx_release(void *c) { note(c, "release"); }

static rec_t R;
static const calib_spin_effects_t FX = { &R, fx_bus_ok, fx_spin, fx_hold, fx_release };

static void reset(bool bus_ok, bool granted) {
    memset(&R, 0, sizeof(R));
    R.bus_ok = bus_ok;
    R.granted = granted;
    R.pair = 0xFF;
}
static void expect(int i, const char *what) {
    if (i >= R.n || strcmp(R.log[i], what) != 0) {
        printf("FAIL effect[%d] = '%s', want '%s'\n", i, i < R.n ? R.log[i] : "(none)", what);
        assert(0);
    }
}
static void want_result(calib_spin_result_t got, calib_spin_result_t want) {
    if (got != want) {
        printf("FAIL result %d, want %d\n", (int)got, (int)want);
        assert(0);
    }
}

int main(void) {
    /* --- the healthy pulse: bus asked, pair spun as asked, held, released, ok ------- */
    reset(true, true);
    want_result(calib_spin_run(&FX, 2, false), CALIB_SPIN_DONE);
    expect(0, "bus?"); expect(1, "spin"); expect(2, "hold"); expect(3, "release");
    assert(R.n == 4);
    assert(R.pair == 2 && R.forward == false);

    /* --- something outranks calibration: 409, and the pulse's hold is not served ---- */
    reset(true, false);
    want_result(calib_spin_run(&FX, 0, true), CALIB_SPIN_REFUSED);
    expect(0, "bus?"); expect(1, "spin");
    assert(R.n == 2);   /* no hold, no release — nothing was granted to release */

    /* --- AJM-100: the bus is down. 409 at once — the arbiter is never asked, so no
           target is set for the actuator to pick up when the boards come back, no
           600 ms hold, nothing to release. The wizard must not advance on this. ------ */
    reset(false, true);
    want_result(calib_spin_run(&FX, 1, true), CALIB_SPIN_BUS_DOWN);
    expect(0, "bus?");
    assert(R.n == 1);
    assert(R.pair == 0xFF);   /* car_spin_pair was not called at all */

    /* --- both refusals wear the same envelope on the wire — 409, code busy, no field —
           and differ only in the message the bench log gets. -------------------------- */
    calib_spin_reply_t bus  = calib_spin_refusal(CALIB_SPIN_BUS_DOWN);
    calib_spin_reply_t held = calib_spin_refusal(CALIB_SPIN_REFUSED);
    assert(strcmp(bus.status,  "409 Conflict") == 0 && strcmp(bus.code,  ERR_BUSY) == 0);
    assert(strcmp(held.status, "409 Conflict") == 0 && strcmp(held.code, ERR_BUSY) == 0);
    assert(bus.msg && held.msg && strcmp(bus.msg, held.msg) != 0);
    assert(strstr(bus.msg, "bus") != NULL);
    /* A finished pulse is not a refusal: no envelope to send. */
    calib_spin_reply_t done = calib_spin_refusal(CALIB_SPIN_DONE);
    assert(done.status == NULL && done.code == NULL && done.msg == NULL);

    printf("test_calib_spin: all passed\n");
    return 0;
}
