#include "../main/cfg_contract.h"
#include "../main/cfg_value.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const cfg_field_t *find(const char *key, const char *name) {
    for (int i = 0; i < CFG_DOMAIN_COUNT; i++) {
        if (strcmp(CFG_DOMAINS[i].key, key) != 0) continue;
        for (int f = 0; f < CFG_DOMAINS[i].n_fields; f++)
            if (strcmp(CFG_DOMAINS[i].fields[f].name, name) == 0) return &CFG_DOMAINS[i].fields[f];
    }
    return NULL;
}

int main(void) {
    const cfg_field_t *gear = find("wheel", "gear_ratio");
    const cfg_field_t *rise = find("ramp", "rise_ms");
    const cfg_field_t *quad = find("wheel", "quadrature");
    const cfg_field_t *en   = find("recovery", "enabled");
    assert(gear && rise && quad && en);
    int32_t v;
    /* fixed: decimal on the wire, integer inside, half away from zero */
    assert(cfg_value_from_double(gear, 9.0, &v) && v == 900);
    /* 9.125 x 100 is exactly 912.5: lround says 913 (half away from zero). 9.005 would
       not do here — it is 9.00499… in binary on both sides, so both agree on 900. */
    assert(cfg_value_from_double(gear, 9.125, &v) && v == 913);
    assert(cfg_value_from_double(gear, 9.124, &v) && v == 912);
    assert(cfg_value_from_double(gear, 300.004, &v) && v == 30000);
    assert(cfg_value_check(gear, 30000) == NULL);
    assert(cfg_value_from_double(gear, 300.0051, &v) && strcmp(cfg_value_check(gear, v), ERR_OUT_OF_RANGE) == 0);
    /* int: a fraction is a type error, not a truncation */
    assert(cfg_value_from_double(rise, 300, &v) && v == 300);
    assert(!cfg_value_from_double(rise, 25.7, &v));
    assert(cfg_value_check(rise, 2000) == NULL);
    assert(strcmp(cfg_value_check(rise, 2001), ERR_OUT_OF_RANGE) == 0);
    assert(strcmp(cfg_value_check(rise, -1), ERR_OUT_OF_RANGE) == 0);
    /* enum: not in the list is not_allowed, not out_of_range */
    assert(cfg_value_from_double(quad, 3, &v) && strcmp(cfg_value_check(quad, v), ERR_NOT_ALLOWED) == 0);
    assert(cfg_value_check(quad, 4) == NULL);
    /* bool: printed as a word */
    char buf[32];
    assert(cfg_value_print(en, 1, buf, sizeof(buf)) > 0 && strcmp(buf, "true") == 0);
    assert(cfg_value_print(en, 0, buf, sizeof(buf)) > 0 && strcmp(buf, "false") == 0);
    assert(cfg_value_print(gear, 900, buf, sizeof(buf)) > 0 && strcmp(buf, "9.00") == 0);
    assert(cfg_value_print(gear, 901, buf, sizeof(buf)) > 0 && strcmp(buf, "9.01") == 0);
    assert(cfg_value_print(gear, 30000, buf, sizeof(buf)) > 0 && strcmp(buf, "300.00") == 0);
    assert(cfg_value_print(rise, 300, buf, sizeof(buf)) > 0 && strcmp(buf, "300") == 0);
    assert(cfg_value_print(find("trim", "balance_pct"), -30, buf, sizeof(buf)) > 0 && strcmp(buf, "-30") == 0);
    assert(cfg_value_print(gear, 900, buf, 3) == -1);
    printf("test_cfg_value: all passed\n");
    return 0;
}
