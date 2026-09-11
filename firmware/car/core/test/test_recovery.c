#include "../main/recovery.h"
#include <assert.h>
#include <stdio.h>
#include <math.h>

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

    printf("test_recovery: all passed\n");
    return 0;
}
