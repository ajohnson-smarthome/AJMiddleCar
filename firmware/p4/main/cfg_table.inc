/* generated from contract/car-api.json by tools/gen_contract.py - do not edit */

#include "cfg_contract.h"

static const int32_t CFG_WHEEL_QUAD_ALLOWED[] = { 1, 2, 4 };

static const cfg_field_t CFG_RAMP_FIELDS[] = {
    { "ramp_ms", CFG_INT, 0, 2000, 300, NULL, 0 },
};
static const cfg_field_t CFG_TRIM_FIELDS[] = {
    { "trim_pct", CFG_INT, -30, 30, 0, NULL, 0 },
};
static const cfg_field_t CFG_RECOVER_FIELDS[] = {
    { "enabled", CFG_BOOL, 0, 1, 1, NULL, 0 },
    { "window_ms", CFG_INT, 1000, 10000, 5000, NULL, 0 },
};
static const cfg_field_t CFG_WHEEL_FIELDS[] = {
    { "diameter_mm", CFG_INT, 20, 150, 65, NULL, 0 },
    { "ppr", CFG_INT, 1, 1000, 11, NULL, 0 },
    { "gear_x100", CFG_INT, 100, 30000, 2100, NULL, 0 },
    { "quad", CFG_ENUM, 1, 4, 4, CFG_WHEEL_QUAD_ALLOWED, 3 },
};
static const cfg_field_t CFG_DIMS_FIELDS[] = {
    { "track_mm", CFG_INT, 60, 300, 130, NULL, 0 },
    { "wheelbase_mm", CFG_INT, 90, 360, 210, NULL, 0 },
};

static const cfg_domain_t CFG_DOMAINS[] = {
    { "/ramp", "ramp", CFG_RAMP_FIELDS, 1 },
    { "/trim", "trim", CFG_TRIM_FIELDS, 1 },
    { "/recover", "recover", CFG_RECOVER_FIELDS, 2 },
    { "/wheel", "wheel", CFG_WHEEL_FIELDS, 4 },
    { "/dims", "dims", CFG_DIMS_FIELDS, 2 },
};

#define CFG_DOMAIN_COUNT 5
#define CFG_MAX_FIELDS 4
