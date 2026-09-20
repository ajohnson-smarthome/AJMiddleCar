// The monitor's verdict as arithmetic (car/battery-monitor): three failed reads in a row
// say `absent`, one good read says `ok` again and starts the remainder over, and `low` is
// a threshold with hysteresis — in at BATTERY_LOW_PCT, out at BATTERY_LOW_CLEAR_PCT. The
// task in battery.c feeds one verdict per read; so does this.
#define BATTERY_HOST_TEST
#include "../main/battery.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int word_is(const battery_rule_t *r, const char *w) { return strcmp(battery_rule_word(r), w) == 0; }

static void test_absent_until_a_read(void) {
    battery_rule_t r;
    battery_rule_init(&r);
    // Before the first read nothing has been measured: the word is `absent`, not `ok` with
    // no numbers. (Change: an init that starts at zero failures.)
    assert(battery_rule_absent(&r));
    assert(word_is(&r, BATTERY_STATE_ABSENT));
    // The first good read: `ok`, remainder still undetermined
    battery_rule_read(&r, -1);
    assert(!battery_rule_absent(&r));
    assert(word_is(&r, BATTERY_STATE_OK));
    printf("  absent until a read ok\n");
}

static void test_three_failures(void) {
    battery_rule_t r;
    battery_rule_init(&r);
    battery_rule_read(&r, 80);
    // One or two failed reads are a NACK on a shared bus, not a missing module: the word
    // holds, and the transition is not reported. (Change: BATTERY_ABSENT_AFTER, or a count
    // that does not reset on success.)
    for (unsigned i = 1; i < BATTERY_ABSENT_AFTER; i++) {
        assert(!battery_rule_failed(&r));
        assert(!battery_rule_absent(&r));
        assert(word_is(&r, BATTERY_STATE_OK));
    }
    // The third in a row: `absent`, and this is the call that says so — once
    assert(battery_rule_failed(&r));
    assert(battery_rule_absent(&r));
    assert(word_is(&r, BATTERY_STATE_ABSENT));
    // Failures keep coming: still absent, never reported again, the counter never wraps
    for (unsigned i = 0; i < 100000; i++) {
        assert(!battery_rule_failed(&r));
        assert(battery_rule_absent(&r));
    }
    // A good read in the middle of a short series clears it: N-1 failures, a read, N-1 more
    // do not add up to absent
    battery_rule_init(&r);
    battery_rule_read(&r, 80);
    for (unsigned i = 1; i < BATTERY_ABSENT_AFTER; i++) battery_rule_failed(&r);
    battery_rule_read(&r, 80);
    for (unsigned i = 1; i < BATTERY_ABSENT_AFTER; i++) assert(!battery_rule_failed(&r));
    assert(!battery_rule_absent(&r));
    printf("  three failures ok\n");
}

static void test_back_from_absent(void) {
    battery_rule_t r;
    battery_rule_init(&r);
    battery_rule_read(&r, 15);
    assert(word_is(&r, BATTERY_STATE_LOW));
    for (unsigned i = 0; i < BATTERY_ABSENT_AFTER; i++) battery_rule_failed(&r);
    assert(word_is(&r, BATTERY_STATE_ABSENT));
    // Back: the remainder starts over (battery_soc_reset in the task, -1 until the start
    // window closes), and `low` does not survive the gap — the pack behind a returning
    // module is not known to be the same pack. (Change: `low` kept across absent.)
    battery_rule_read(&r, -1);
    assert(!battery_rule_absent(&r));
    assert(word_is(&r, BATTERY_STATE_OK));
    printf("  back from absent ok\n");
}

static void test_low_hysteresis(void) {
    battery_rule_t r;
    battery_rule_init(&r);
    // The spec's scenario in numbers: 20 → low; 21 after a rest pull-in → still low; 23 → ok.
    // (Change: either threshold, or a single threshold without memory.)
    battery_rule_read(&r, BATTERY_LOW_PCT + 1);
    assert(word_is(&r, BATTERY_STATE_OK));
    battery_rule_read(&r, BATTERY_LOW_PCT);
    assert(word_is(&r, BATTERY_STATE_LOW));
    battery_rule_read(&r, BATTERY_LOW_PCT + 1);
    assert(word_is(&r, BATTERY_STATE_LOW));
    battery_rule_read(&r, BATTERY_LOW_CLEAR_PCT - 1);
    assert(word_is(&r, BATTERY_STATE_LOW));
    battery_rule_read(&r, BATTERY_LOW_CLEAR_PCT);
    assert(word_is(&r, BATTERY_STATE_OK));
    // and from ok, the band between the thresholds is still ok
    battery_rule_read(&r, BATTERY_LOW_CLEAR_PCT - 1);
    assert(word_is(&r, BATTERY_STATE_OK));
    battery_rule_read(&r, BATTERY_LOW_PCT + 1);
    assert(word_is(&r, BATTERY_STATE_OK));
    // Straight down to zero is low; straight up to full is ok
    battery_rule_read(&r, 0);
    assert(word_is(&r, BATTERY_STATE_LOW));
    battery_rule_read(&r, 100);
    assert(word_is(&r, BATTERY_STATE_OK));
    printf("  low hysteresis ok\n");
}

static void test_unknown_is_ok(void) {
    battery_rule_t r;
    battery_rule_init(&r);
    // While the remainder is undetermined the word is `ok`: a null soc_pct is not a low pack.
    battery_rule_read(&r, -1);
    assert(word_is(&r, BATTERY_STATE_OK));
    // ...and an undetermined remainder after a low one is `ok` too: -1 is "not known", and
    // `low` is a statement about a number
    battery_rule_read(&r, 10);
    assert(word_is(&r, BATTERY_STATE_LOW));
    battery_rule_read(&r, -1);
    assert(word_is(&r, BATTERY_STATE_OK));
    printf("  unknown is ok ok\n");
}

int main(void) {
    _Static_assert(BATTERY_ABSENT_AFTER >= 2, "one NACK on a shared bus is not a missing module");
    _Static_assert(BATTERY_LOW_CLEAR_PCT > BATTERY_LOW_PCT, "hysteresis needs a band");
    _Static_assert(BATTERY_PERIOD_MS <= 200, "car/battery-monitor: not less than five reads a second");
    test_absent_until_a_read();
    test_three_failures();
    test_back_from_absent();
    test_low_hysteresis();
    test_unknown_is_ok();
    printf("test_battery: OK\n");
    return 0;
}
