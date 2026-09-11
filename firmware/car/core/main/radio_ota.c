#include "radio_ota.h"

#include <string.h>

/* What the version read leaves in place when the RPC fails. Compared here rather than guarded
 * at the call site, so the refusal is part of the tested decision and not of the wiring. */
#define RADIO_FW_UNAVAILABLE "unavailable"

bool radio_ota_should_flash(const char *running, const char *expected,
                            int attempts, int max_attempts, bool have_image)
{
    if (!have_image)                                   return false;
    if (running == NULL || expected == NULL)           return false;
    if (strcmp(running, RADIO_FW_UNAVAILABLE) == 0)    return false;
    if (strcmp(running, expected) == 0)                return false;
    return attempts < max_attempts;
}

int radio_ota_next_attempts(bool versions_match, int attempts)
{
    if (versions_match)                        return 0;
    if (attempts >= RADIO_OTA_MAX_ATTEMPTS)    return RADIO_OTA_MAX_ATTEMPTS;
    return attempts + 1;
}
