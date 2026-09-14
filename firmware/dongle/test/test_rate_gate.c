#include "../main/rate_gate.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    rate_gate_t g;
    rate_gate_init(&g, 2500, 100);            /* 2500 kbit/s over 100 ms = 31250 bytes per window */
    uint32_t t = 1000;
    for (int i = 0; i < 22; i++) assert(rate_gate_admit(&g, t, 1412));   /* 31064 */
    assert(!rate_gate_admit(&g, t + 50, 1412));                          /* 32476 > 31250 */
    assert(rate_gate_admit(&g, t + 50, 100));                            /* a small one still fits */
    assert(!rate_gate_admit(&g, t + 99, 1412));
    assert(rate_gate_admit(&g, t + 100, 1412));                          /* a new window */
    /* A refusal does not consume budget. */
    rate_gate_init(&g, 100, 100);                                        /* 1250 bytes per window */
    assert(!rate_gate_admit(&g, 0, 1300));
    assert(rate_gate_admit(&g, 0, 1250));
    /* Zero limit admits nothing; the clock wrapping is not a new window. */
    rate_gate_init(&g, 0, 100);
    assert(!rate_gate_admit(&g, 5, 1));
    rate_gate_init(&g, 2500, 100);
    assert(rate_gate_admit(&g, 0xFFFFFFF0u, 31250));
    assert(!rate_gate_admit(&g, 0xFFFFFFF0u + 50, 1));
    assert(rate_gate_admit(&g, 0xFFFFFFF0u + 100, 1));
    printf("rate_gate: ok\n");
    return 0;
}
