#include "rate_gate.h"

void rate_gate_init(rate_gate_t *g, uint32_t limit_kbps, uint32_t window_ms)
{
    g->limit_kbps = limit_kbps;
    g->window_ms = window_ms;
    g->window_start_ms = 0;
    g->bytes = 0;
}

bool rate_gate_admit(rate_gate_t *g, uint32_t now_ms, uint32_t bytes)
{
    if ((uint32_t)(now_ms - g->window_start_ms) >= g->window_ms) {
        g->window_start_ms = now_ms;
        g->bytes = 0;
    }
    /* kbit/s x ms = bits per window; 8 x bytes = bits. uint64_t: 3000 x 100 x 8 is fine in
     * 32 bits, but the limit is a contract number and contracts grow. */
    uint64_t budget_bits = (uint64_t)g->limit_kbps * g->window_ms;
    uint64_t used_bits = ((uint64_t)g->bytes + bytes) * 8u;
    if (used_bits > budget_bits) return false;
    g->bytes += bytes;
    return true;
}
