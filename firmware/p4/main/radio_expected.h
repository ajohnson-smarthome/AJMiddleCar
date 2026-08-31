#pragma once
#include "eh_common_fw_version.h"   /* PROJECT_VERSION_MAJOR_1/MINOR_1/PATCH_1 live here; without
                                     * this include an includer that never pulls it in some other
                                     * way would stringify the bare macro names instead of their
                                     * values — main.c is exactly that includer. */
/* The expected slave version is the HOST library's version: esp_hosted requires the pair
 * matched, and deriving the string from the component's own macros makes drift between
 * idf_component.yml and this check impossible. Shared between status_api.c, which reports it,
 * and main.c's boot gate, which acts on it. */
#define RADIO_STR2(x) #x
#define RADIO_STR(x)  RADIO_STR2(x)
#define RADIO_EXPECTED_FW \
    RADIO_STR(PROJECT_VERSION_MAJOR_1) "." RADIO_STR(PROJECT_VERSION_MINOR_1) "." RADIO_STR(PROJECT_VERSION_PATCH_1)
