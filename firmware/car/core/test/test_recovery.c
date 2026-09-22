#include "../main/recovery.h"
#include "../main/contract.h"   /* the recovery domain's range, RT_WATCHDOG_MS, RT_SESSION_IDLE_MS */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

static const cfg_field_t *window_field(void) {
    for (int i = 0; i < CFG_DOMAIN_COUNT; i++) {
        if (strcmp(CFG_DOMAINS[i].key, "recovery") != 0) continue;
        for (int f = 0; f < CFG_DOMAINS[i].n_fields; f++) {
            if (strcmp(CFG_DOMAINS[i].fields[f].name, "window_ms") == 0) {
                return &CFG_DOMAINS[i].fields[f];
            }
        }
    }
    return NULL;
}

static int feq(float a, float b) { return fabsf(a - b) < 1e-6f; }

int main(void) {
    // recovery_reverse negates both axes
    float rt, ry;
    recovery_reverse(0.8f, -0.3f, &rt, &ry);
    assert(feq(rt, -0.8f) && feq(ry, 0.3f));
    recovery_reverse(0.0f, 0.0f, &rt, &ry);
    assert(feq(rt, 0.0f) && feq(ry, 0.0f));

    // recovery_evict: sample is evicted when older than the window
    assert(recovery_evict(0, 6000, 5000) == true);    // 6s old, 5s window → evict
    assert(recovery_evict(2000, 6000, 5000) == false); // 4s old, 5s window → keep
    assert(recovery_evict(6000, 6000, 5000) == false); // same instant → keep
    // unsigned-rollover safe: now wrapped past UINT32_MAX
    assert(recovery_evict(0xFFFFFF00u, 0x00000064u, 5000) == false); // 356ms apart → keep
    assert(recovery_evict(0xFFFF0000u, 0x00010000u, 5000) == true);  // ~131s apart → evict

    /* One replay segment: the gap between breadcrumb timestamps, capped. The gap is
       drive time only while frames flowed at RT_COMMAND_HZ; across a refusal gap (a
       wizard pulse, an OTA hold) or the open tail it is dead air, and crediting it
       replayed a crawl as seconds of reverse. */
    assert(recovery_seg_ms(1100, 1000) == 100);
    assert(recovery_seg_ms(1000, 1000) == 0);
    assert(recovery_seg_ms(5000, 1000) == RECOVER_SEG_MAX_MS);
    assert(recovery_seg_ms(250 + 7, 7) == 250);
    assert(recovery_seg_ms(10, 0xFFFFFF00u) == RECOVER_SEG_MAX_MS);  /* rollover-safe */

    /* What counts as path (AJM-169): a command the actuator took AND whose write would
       reach the wheels. The grant asks the arbiter only; with the PWM boards down the
       write is swallowed in link.c, and a breadcrumb then is a path never driven. */
    assert(recovery_is_breadcrumb(true, true) == true);
    assert(recovery_is_breadcrumb(true, false) == false);   /* granted into a dead bus */
    assert(recovery_is_breadcrumb(false, true) == false);   /* refused by the arbiter */
    assert(recovery_is_breadcrumb(false, false) == false);

    /* When a lost link may retrace at all: recovery on AND a live bus. The boards can drop
       out mid-drive after real motion was recorded — retracing that on dead wheels is
       `recovering` for a car that stands, so it is a plain stop, as with recovery off. */
    assert(recovery_may_retrace(true, true) == true);
    assert(recovery_may_retrace(true, false) == false);     /* boards dropped out */
    assert(recovery_may_retrace(false, true) == false);     /* recovery off */
    assert(recovery_may_retrace(false, false) == false);

    /* The window's bounds are the contract's: recovery.h spells them because it is pure
       and includes nothing, so this is what keeps the two from drifting apart. */
    const cfg_field_t *w = window_field();
    assert(w && w->type == CFG_INT);
    assert(RECOVER_WIN_MIN_MS == w->min);
    assert(RECOVER_WIN_MAX_MS == w->max);

    /* Two budgets on one axis (AJM-129). A retrace over the full window starts
       RT_WATCHDOG_MS after the last accepted command, replays a tail capped at
       RECOVER_SEG_MAX_MS and then gaps that add up to at most the window; the session
       dies strictly past RT_SESSION_IDLE_MS from that same command and throws the path
       away, retrace and all. Session mortality is the senior budget, so the contract
       keeps the window's ceiling under it with room for ticks and scheduling — at
       10 000 the session's death cut the oldest half-second of the path instead of the
       retrace finishing it. */
    enum { SLACK_MS = 1000 };
    assert(RT_WATCHDOG_MS + RECOVER_SEG_MAX_MS + RECOVER_WIN_MAX_MS + SLACK_MS < RT_SESSION_IDLE_MS);

    /* What a stored window becomes at boot, and what recovery_set_config holds: clamped
       to the contract's range, not reset to the default. The ceiling came down from
       10 000 to 8 000, and a car whose owner had chosen the old ceiling should boot
       with the new one rather than with 5 000. */
    assert(recovery_window_clamp(10000) == 8000);
    assert(recovery_window_clamp(8001) == 8000);
    assert(recovery_window_clamp(8000) == 8000);
    assert(recovery_window_clamp(5000) == 5000);
    assert(recovery_window_clamp(1000) == 1000);
    assert(recovery_window_clamp(999) == 1000);
    assert(recovery_window_clamp(-1) == 1000);
    assert(recovery_window_clamp(0x7fffffff) == 8000);

    printf("test_recovery: all passed\n");
    return 0;
}
