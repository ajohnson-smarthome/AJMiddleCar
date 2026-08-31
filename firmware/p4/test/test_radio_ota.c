/* The decision that keeps a car recoverable. Every case here is a way the car could end up
 * unable to boot into a working state with no cable in reach, so the test is about refusals
 * as much as about the one path that flashes. */
#include <stdio.h>
#include <string.h>
#include "radio_ota.h"

static int failures;
static void check(int ok, const char *what) {
    if (!ok) { printf("FAIL: %s\n", what); failures++; }
}

int main(void) {
    const int MAX = 3;

    /* The one case that acts. */
    check(radio_ota_should_flash("2.11.7", "3.0.6", 0, MAX, true),
          "a mismatch with budget left and an image on board flashes");
    check(radio_ota_should_flash("2.11.7", "3.0.6", 2, MAX, true),
          "the last attempt in the budget still flashes");

    /* A build with no image can do nothing about a mismatch, and must not pretend. */
    check(!radio_ota_should_flash("2.11.7", "3.0.6", 0, MAX, false),
          "no embedded image never flashes");

    /* An unknown is not a mismatch. read_radio_version leaves "unavailable" when the RPC
     * fails, and flashing on that would mean flashing a radio we could not talk to. */
    check(!radio_ota_should_flash("unavailable", "3.0.6", 0, MAX, true),
          "an unreadable running version never flashes");
    check(!radio_ota_should_flash(NULL, "3.0.6", 0, MAX, true),
          "a null running version never flashes");
    check(!radio_ota_should_flash("2.11.7", NULL, 0, MAX, true),
          "a null expected version never flashes");

    /* Matching is the ordinary path and must cost nothing. */
    check(!radio_ota_should_flash("3.0.6", "3.0.6", 0, MAX, true),
          "a matched pair does not flash");
    check(!radio_ota_should_flash("3.0.6", "3.0.6", 2, MAX, true),
          "a matched pair does not flash even with attempts on the clock");

    /* The budget is what stops a boot loop nobody can reach with a cable. */
    check(!radio_ota_should_flash("2.11.7", "3.0.6", 3, MAX, true),
          "a spent budget stops trying");
    check(!radio_ota_should_flash("2.11.7", "3.0.6", 99, MAX, true),
          "a counter past the budget stops trying");

    /* The counter: a match is the only thing that clears it, and it is cleared even when the
     * car never flashed anything — a radio matched by a bench reflash must not leave the
     * budget spent for the next genuine mismatch. */
    check(radio_ota_next_attempts(true, 0) == 0, "a match keeps a clean counter clean");
    check(radio_ota_next_attempts(true, 3) == 0, "a match clears a spent counter");
    check(radio_ota_next_attempts(false, 0) == 1, "a mismatch charges the first attempt");
    check(radio_ota_next_attempts(false, 2) == 3, "a mismatch charges the last attempt");
    check(radio_ota_next_attempts(false, 3) == 3,
          "the counter saturates rather than wrapping — it is stored as one byte");

    if (!failures) printf("test_radio_ota: all passed\n");
    return failures ? 1 : 0;
}
