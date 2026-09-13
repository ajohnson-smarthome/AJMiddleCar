#ifndef CALIB_WIRE_H
#define CALIB_WIRE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "contract.h"
#include "motors.h"

/* The wire's words for the calibration table, mapped to motors.h's positions. The table
 * is stored by position (calibration.c) and spoken by corner name; this is the edge. */

static inline const char *calib_corner_word(int pos) {
    switch (pos) {
        case POS_FL: return CORNER_FRONT_LEFT;
        case POS_FR: return CORNER_FRONT_RIGHT;
        case POS_RL: return CORNER_REAR_LEFT;
        case POS_RR: return CORNER_REAR_RIGHT;
        default:     return NULL;
    }
}

static inline int calib_corner_index(const char *word) {
    for (int p = 0; p < POS_COUNT; p++) {
        if (strcmp(calib_corner_word(p), word) == 0) return p;
    }
    return -1;
}

/* 1 forward, 0 reverse, -1 for a word the wire does not use. */
static inline int calib_direction_forward(const char *word) {
    if (strcmp(word, DIRECTION_FORWARD) == 0) return 1;
    if (strcmp(word, DIRECTION_REVERSE) == 0) return 0;
    return -1;
}

/* "calibrated":…,"wheels":[…] — the GET /calibration members, and the POST's reply. An
 * uncalibrated car answers an empty table rather than the defaults it is not using. */
static inline int calib_table_json(char *buf, size_t n, bool calibrated, const motors_config_t *cfg) {
    int r = snprintf(buf, n, "\"" KEY_CALIB_CALIBRATED "\":%s,\"" KEY_CALIB_WHEELS "\":[",
                     calibrated ? "true" : "false");
    if (r < 0 || r >= (int)n) return -1;
    size_t at = (size_t)r;
    if (calibrated) {
        for (int p = 0; p < POS_COUNT; p++) {
            int w = snprintf(buf + at, n - at,
                             "%s{\"" KEY_CALIB_CORNER "\":\"%s\",\"" KEY_CALIB_PAIR "\":%u,"
                             "\"" KEY_CALIB_INVERTED "\":%s}",
                             p ? "," : "", calib_corner_word(p),
                             (unsigned)cfg->wheels[p].channel_pair,
                             cfg->wheels[p].sign < 0 ? "true" : "false");
            if (w < 0 || (size_t)w >= n - at) return -1;
            at += (size_t)w;
        }
    }
    if (at + 2 > n) return -1;
    buf[at++] = ']';
    buf[at] = '\0';
    return (int)at;
}

#endif /* CALIB_WIRE_H */
