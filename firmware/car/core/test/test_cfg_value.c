#include "../main/cfg_contract.h"
#include "../main/cfg_value.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const cfg_domain_t *domain(const char *key) {
    for (int i = 0; i < CFG_DOMAIN_COUNT; i++)
        if (strcmp(CFG_DOMAINS[i].key, key) == 0) return &CFG_DOMAINS[i];
    return NULL;
}

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

    /* A stored record at boot, field by field (car/config, «При старте — из памяти, иначе
       умолчания контракта»): what the record named is what the car starts with, what it
       did not name is the contract's default — and one does not cost the other. wheel_init
       and dims_init used to demand every field or throw the record away, so a firmware
       that added a field to the domain reset the three the user had chosen (AJM-104). */
    const cfg_domain_t *wheel = domain("wheel");
    const cfg_domain_t *chassis = domain("chassis");
    assert(wheel && wheel->n_fields == 4 && chassis && chassis->n_fields == 2);
    int32_t out[CFG_MAX_FIELDS];
    {   /* a record older than the fourth field: the three it named survive */
        bool has[4] = { true, true, true, false };
        int32_t v[4] = { 70, 13, 1000, 0 };
        cfg_values_stored(wheel->fields, wheel->n_fields, has, v, out);
        assert(out[0] == 70 && out[1] == 13 && out[2] == 1000 && out[3] == 4);
    }
    {   /* the missing field may be any of them, not just the last */
        bool has[4] = { false, true, true, true };
        int32_t v[4] = { 0, 13, 1000, 2 };
        cfg_values_stored(wheel->fields, wheel->n_fields, has, v, out);
        assert(out[0] == 65 && out[1] == 13 && out[2] == 1000 && out[3] == 2);
    }
    {   /* no record, or one that did not parse: every field the default */
        bool has[4] = { false, false, false, false };
        int32_t v[4] = { 1, 2, 3, 5 };
        cfg_values_stored(wheel->fields, wheel->n_fields, has, v, out);
        assert(out[0] == 65 && out[1] == 11 && out[2] == 900 && out[3] == 4);
    }
    {   /* a stored value outside the range does not take effect as is: held to the
           bound, on its own — 65556 was (uint16_t)20 once, and then threw away the
           record. An enum outside its list has no nearest value: the default. */
        bool has[4] = { true, true, true, true };
        int32_t v[4] = { 65556, 0, 900, 3 };
        cfg_values_stored(wheel->fields, wheel->n_fields, has, v, out);
        assert(out[0] == 150 && out[1] == 1 && out[2] == 900 && out[3] == 4);
    }
    {   /* chassis: a record with the track only keeps the track */
        bool has[2] = { true, false };
        int32_t v[2] = { 140, 0 };
        cfg_values_stored(chassis->fields, chassis->n_fields, has, v, out);
        assert(out[0] == 140 && out[1] == 210);
    }
    printf("test_cfg_value: all passed\n");
    return 0;
}
