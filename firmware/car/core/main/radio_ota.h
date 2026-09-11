#pragma once
#include <stdbool.h>

/* Whether to push the embedded radio image at the co-processor, and how the attempt counter
 * moves. Pure on purpose: this is the decision that determines whether a car in the field can
 * be recovered without a cable, and it is worth being able to check by reading rather than by
 * reproducing on hardware.
 *
 * The counter is NOT about the write succeeding. A failed write is harmless — the slave's OTA
 * lands in its inactive slot — so what is bounded here is the LOOP: flash, reboot, still
 * mismatched, flash again. The authority on success is the version read after the reboot, and
 * nothing else.
 */

/* Three tries, then live with a mismatched radio — which is exactly what the car does today,
 * and today's behaviour is a car that drives. Giving up is the safe direction. */
#define RADIO_OTA_MAX_ATTEMPTS 3

/* Refuses on every unknown. An unreadable running version is not a mismatch, a build with no
 * image cannot act on one, and a spent budget must stop rather than loop. */
bool radio_ota_should_flash(const char *running, const char *expected,
                            int attempts, int max_attempts, bool have_image);

/* The counter's next value. A match clears it — including when the match came from a bench
 * reflash rather than from anything this car did, so the budget is fresh for the next real
 * mismatch. A mismatch charges one and saturates at RADIO_OTA_MAX_ATTEMPTS, because it is
 * stored as a single NVS byte and wrapping would silently re-arm the loop. */
int radio_ota_next_attempts(bool versions_match, int attempts);
