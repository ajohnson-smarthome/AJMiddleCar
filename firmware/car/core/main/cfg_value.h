#ifndef CFG_VALUE_H
#define CFG_VALUE_H

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "cfg_contract.h"
#include "contract.h"

/* A JSON number -> the integer the descriptor holds. false when the number is not the
 * field's kind: an int or enum given a fraction (cJSON's valueint would TRUNCATE 25.7 to
 * 25 under a 200, and the generated validator answers wrong_type for the same bytes).
 * CFG_FIXED takes any finite number and rounds half away from zero, as lround does and
 * as the mock's lround does. Booleans never reach here — cJSON_IsBool is checked first. */
static inline bool cfg_value_from_double(const cfg_field_t *f, double v, int32_t *out) {
    if (!isfinite(v)) return false;
    if (f->type == CFG_FIXED) {
        double s = v * (double)f->scale;
        if (s > 2147483647.0 || s < -2147483648.0) return false;
        *out = (int32_t)lround(s);
        return true;
    }
    if (v != (double)(int32_t)v) return false;
    *out = (int32_t)v;
    return true;
}

/* NULL when `v` is acceptable for `f`, else the ERR_* word that names why. */
static inline const char *cfg_value_check(const cfg_field_t *f, int32_t v) {
    if (f->type == CFG_ENUM) {
        for (uint8_t k = 0; k < f->n_allowed; k++) if (f->allowed[k] == v) return NULL;
        return ERR_NOT_ALLOWED;
    }
    if (v < f->min || v > f->max) return ERR_OUT_OF_RANGE;
    return NULL;
}

/* The wire spelling of a held value: true/false, a decimal with scale's digits, or an
 * integer. Returns the length, or -1 when it does not fit. */
static inline int cfg_value_print(const cfg_field_t *f, int32_t v, char *buf, size_t n) {
    int r;
    if (f->type == CFG_BOOL) {
        r = snprintf(buf, n, "%s", v ? "true" : "false");
    } else if (f->type == CFG_FIXED) {
        int digits = 0;
        for (int32_t s = f->scale; s > 1; s /= 10) digits++;
        int32_t whole = v / f->scale, frac = v % f->scale;
        if (frac < 0) frac = -frac;
        if (v < 0 && whole == 0) r = snprintf(buf, n, "-0.%0*ld", digits, (long)frac);
        else                     r = snprintf(buf, n, "%ld.%0*ld", (long)whole, digits, (long)frac);
    } else {
        r = snprintf(buf, n, "%ld", (long)v);
    }
    if (r < 0 || r >= (int)n) return -1;
    return r;
}

#endif /* CFG_VALUE_H */
