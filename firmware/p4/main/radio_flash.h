#pragma once
#include <stddef.h>

/* Pushing the embedded co-processor image over the SDIO link that already exists, and reading
 * back what the radio runs. No policy lives here — whether to do it at all is radio_ota.h's
 * decision, and that one is pure and tested.
 *
 * The route is the vendor's: esp_hosted_cp_ota_begin/write/end/activate, reached through the
 * component's compat header, and proven on this board on 2026-08-20 (firmware/c6/README.md).
 */

/* The co-processor's running version, e.g. "3.0.6". Read once over RPC on the first call and
 * cached: against a mismatched slave that call costs up to five seconds, and the answer cannot
 * change without a reboot. Returns "unavailable" when the RPC failed. */
const char *radio_flash_version(void);

/* How many bytes of radio image this build carries. Zero means it carries none — an ordinary
 * developer build — and nothing should be attempted. */
size_t radio_flash_image_size(void);

/* Push the embedded image at the co-processor and restart the car. Does not return.
 *
 * Two documented vendor behaviours are treated as success rather than as errors: activate()
 * returning ESP_FAIL against a slave older than v2.6.0 (the old image applies the update itself
 * on end()), and the SDIO link dropping right after end() (that is the radio rebooting into its
 * new firmware). In both cases the verdict belongs to the version read on the next boot, not to
 * a return code — so this restarts either way, and the caller has already charged the attempt.
 */
void radio_flash_apply(void);
