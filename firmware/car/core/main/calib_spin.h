#ifndef CALIB_SPIN_H
#define CALIB_SPIN_H

#include <stdint.h>
#include <stdbool.h>
#include "contract.h"   /* ERR_BUSY */

/* POST /calibration/spin once its body has been read and validated: whether the pulse
 * may go ahead, and the order of what follows, over an effects table instead of the live
 * modules — calib_api.c supplies the real table, the host test a recorder. The same seam
 * rt_glue.h is for the session lifecycle: the handler's sequence is a few impure calls in
 * a row, and which of them run on a refusal is exactly what the wizard's 409 promises. */

typedef struct {
    void *ctx;                                            /* the recorder in tests; NULL in firmware */
    bool (*bus_ok)(void *ctx);                            /* link_bus_ok() */
    bool (*spin)(void *ctx, uint8_t pair, bool forward);  /* car_spin_pair() */
    void (*hold)(void *ctx);                              /* vTaskDelay(LINK_HOLD_CALIB_MS) */
    void (*release)(void *ctx);                           /* link_release_must(LINK_SRC_CALIB) */
} calib_spin_effects_t;

typedef enum {
    CALIB_SPIN_DONE,       /* pulsed, held, released: 200 ok */
    CALIB_SPIN_BUS_DOWN,   /* motors.bus is down: 409 busy, the arbiter never asked */
    CALIB_SPIN_REFUSED,    /* something outranks calibration: 409 busy, nothing granted */
} calib_spin_result_t;

/* The bus is asked BEFORE the arbiter, and a "down" ends the request there: no grant, so
 * no target for the actuator to pick up if the boards come back mid-hold, no
 * LINK_HOLD_CALIB_MS of waiting, nothing to release. Asking afterwards was the bug (AJM-100): the grant went through,
 * link.c skipped every write behind pca9685_ready(), the hold lapsed, and the handler
 * said ok about a wheel that had not moved — which the wizard took as "pick the wheel". */
static inline calib_spin_result_t calib_spin_run(const calib_spin_effects_t *fx,
                                                 uint8_t pair, bool forward) {
    if (!fx->bus_ok(fx->ctx)) return CALIB_SPIN_BUS_DOWN;
    if (!fx->spin(fx->ctx, pair, forward)) return CALIB_SPIN_REFUSED;
    fx->hold(fx->ctx);
    fx->release(fx->ctx);
    return CALIB_SPIN_DONE;
}

typedef struct {
    const char *status;   /* the HTTP status line */
    const char *code;     /* one of the contract's ERR_* words */
    const char *msg;      /* the envelope's message — for the log, not the screen */
} calib_spin_reply_t;

/* The envelope a refused pulse gets. Both refusals are 409 busy without a field — the
 * request was fine, the car cannot serve it right now, and the wheel did not turn — so
 * the client's move is the same; only the bench log learns which. A finished pulse is not
 * a refusal and gets no envelope (all NULL). */
static inline calib_spin_reply_t calib_spin_refusal(calib_spin_result_t r) {
    switch (r) {
    case CALIB_SPIN_BUS_DOWN: return (calib_spin_reply_t){ "409 Conflict", ERR_BUSY, "motor bus down" };
    case CALIB_SPIN_REFUSED:  return (calib_spin_reply_t){ "409 Conflict", ERR_BUSY, "actuator busy" };
    case CALIB_SPIN_DONE:
    default:                  return (calib_spin_reply_t){ NULL, NULL, NULL };
    }
}

#endif /* CALIB_SPIN_H */
