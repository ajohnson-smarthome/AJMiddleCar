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
    check(radio_ota_should_flash("2.11.7", "3.0.6", 0, MAX, true, true),
          "a mismatch with budget left and an image on board flashes");
    check(radio_ota_should_flash("2.11.7", "3.0.6", 2, MAX, true, true),
          "the last attempt in the budget still flashes");

    /* A build with no image can do nothing about a mismatch, and must not pretend. */
    check(!radio_ota_should_flash("2.11.7", "3.0.6", 0, MAX, false, true),
          "no embedded image never flashes");

    /* An unknown is not a mismatch. read_radio_version leaves "unavailable" when the RPC
     * fails, and flashing on that would mean flashing a radio we could not talk to. */
    check(!radio_ota_should_flash("unavailable", "3.0.6", 0, MAX, true, true),
          "an unreadable running version never flashes");
    check(!radio_ota_should_flash(NULL, "3.0.6", 0, MAX, true, true),
          "a null running version never flashes");
    check(!radio_ota_should_flash("2.11.7", NULL, 0, MAX, true, true),
          "a null expected version never flashes");

    /* Matching is the ordinary path and must cost nothing. */
    check(!radio_ota_should_flash("3.0.6", "3.0.6", 0, MAX, true, true),
          "a matched pair does not flash");
    check(!radio_ota_should_flash("3.0.6", "3.0.6", 2, MAX, true, true),
          "a matched pair does not flash even with attempts on the clock");

    /* The budget is what stops a boot loop nobody can reach with a cable. */
    check(!radio_ota_should_flash("2.11.7", "3.0.6", 3, MAX, true, true),
          "a spent budget stops trying");
    check(!radio_ota_should_flash("2.11.7", "3.0.6", 99, MAX, true, true),
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

    /* The budget belongs to one target. The counter was charged against a particular expected
     * version and is stored beside it; a build that expects a different one — a release with a
     * newer radio pin, or the same pin behind a fixed delivery route — starts fresh. Without
     * this, three interrupted deliveries left a car unable to accept the very release that
     * could have fixed its radio (AJM-97). The same target keeps its saturated counter, so the
     * loop stays bounded at three per version. */
    check(radio_ota_attempts_for("3.0.6", "3.0.6", 3) == 3,
          "the same target keeps its spent counter");
    check(radio_ota_attempts_for("3.0.6", "3.0.6", 1) == 1,
          "the same target keeps a partly spent counter");
    check(radio_ota_attempts_for("3.0.6", "3.0.7", 3) == 0,
          "a different target starts with a fresh budget");
    check(radio_ota_attempts_for(NULL, "3.0.7", 3) == 0,
          "a counter with no record of its target is a fresh budget — an older firmware's byte");
    check(radio_ota_attempts_for("", "3.0.7", 3) == 0,
          "an empty target record is a fresh budget");
    check(radio_ota_should_flash("2.11.7", "3.0.7",
                                 radio_ota_attempts_for("3.0.6", "3.0.7", 3), MAX, true, true),
          "a release expecting a different radio flashes after three spent on the old one");
    check(!radio_ota_should_flash("2.11.7", "3.0.6",
                                  radio_ota_attempts_for("3.0.6", "3.0.6", 3), MAX, true, true),
          "the same expected version stays saturated");

    /* The gate's restart is one the car chose, and the bootloader cannot tell it from a crash:
     * under PENDING_VERIFY it reverts the image. An image whose confirmation failed therefore
     * keeps the gate shut this boot, whatever the versions say (AJM-138). */
    check(!radio_ota_should_flash("2.11.7", "3.0.6", 0, MAX, true, false),
          "an unconfirmed image never flashes — its restart would roll the image back");

    if (!failures) printf("test_radio_ota: all passed\n");
    return failures ? 1 : 0;
}
