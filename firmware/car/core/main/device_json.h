#ifndef DEVICE_JSON_H
#define DEVICE_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "contract.h"
#include "identity.h"

/* The `device` group — who is answering — printed once, for the hello reply and for
 * /status. One printer, so the two cannot spell the identity differently and present
 * on the phone as "wrong car". Pure: host-tested in test_device_json.c. */

/* The number after '+' in a version like "v1.0+784", or -1 when there is none. The app
 * compares this integer against the release it knows; the tag parse used to be its job. */
static inline int fw_build_number(const char *fw) {
    const char *plus = strchr(fw, '+');
    if (!plus || plus[1] < '0' || plus[1] > '9') return -1;
    int n = 0;
    for (const char *p = plus + 1; *p >= '0' && *p <= '9'; p++) {
        if (n > 214748363) return -1;   /* would not fit an int */
        n = n * 10 + (*p - '0');
    }
    return n;
}

/* "device":{...} — no braces around, no trailing comma. Returns the length, or -1 when
 * it does not fit: a truncated identity parses as a different car, so a caller must
 * refuse to send rather than send what fits. */
static inline int device_group_json(char *buf, size_t n, const char *fw, bool rolled_back) {
    int r = snprintf(buf, n,
        "\"" KEY_GROUP_DEVICE "\":{\"" KEY_DEVICE_ID "\":\"" CAR_DEVICE_ID "\","
        "\"" KEY_DEVICE_FW "\":\"%s\",\"" KEY_DEVICE_BUILD "\":%d,"
        "\"" KEY_DEVICE_ROLLED_BACK "\":%s}",
        fw, fw_build_number(fw), rolled_back ? "true" : "false");
    if (r < 0 || r >= (int)n) return -1;
    return r;
}

#endif /* DEVICE_JSON_H */
