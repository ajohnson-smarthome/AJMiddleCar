#include "watchdog.h"
#include <assert.h>
#include <stdio.h>

static void check(uint32_t last, uint32_t now, uint32_t to, bool want) {
    bool got = watchdog_stale(last, now, to);
    if (got != want) {
        printf("FAIL stale(%u,%u,%u) = %d, want %d\n", last, now, to, got, want);
        assert(0);
    }
}

int main(void) {
    check(100, 300, 300, false);   // 200ms elapsed, not yet stale
    check(100, 400, 300, false);   // exactly 300ms — strict '>' so not stale
    check(100, 401, 300, true);    // 301ms — stale
    check(0, 0, 300, false);       // no time passed
    check(0, 5000, 300, true);     // long gap
    // 32-bit rollover: last near max, now wrapped past zero
    check(0xFFFFFF00u, 0x00000050u, 300, true);   // (0x50 - 0xFFFFFF00) = 0x150 = 336 > 300
    check(0xFFFFFF00u, 0xFFFFFF00u + 100u, 300, false); // 100ms across wrap, not stale
    printf("test_watchdog: all passed\n");
    return 0;
}
