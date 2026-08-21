/* The generated table must compile as plain C and carry the schema's numbers. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "cfg_contract.h"
#include "cfg_table.inc"

static const cfg_field_t *find(const char *path, const char *name) {
    for (int i = 0; i < CFG_DOMAIN_COUNT; i++) {
        if (strcmp(CFG_DOMAINS[i].path, path) != 0) continue;
        for (int f = 0; f < CFG_DOMAINS[i].n_fields; f++) {
            if (strcmp(CFG_DOMAINS[i].fields[f].name, name) == 0) {
                return &CFG_DOMAINS[i].fields[f];
            }
        }
    }
    return NULL;
}

int main(void) {
    assert(CFG_DOMAIN_COUNT == 5);
    /* The generic handler sizes a stack array from this; a sixth field in the
       schema must break the build, not the runtime. */
    assert(CFG_MAX_FIELDS == 4);
    for (int i = 0; i < CFG_DOMAIN_COUNT; i++) assert(CFG_DOMAINS[i].n_fields <= CFG_MAX_FIELDS);

    const cfg_field_t *d = find("/wheel", "diameter_mm");
    assert(d && d->type == CFG_INT && d->min == 20 && d->max == 150 && d->def == 65);

    const cfg_field_t *q = find("/wheel", "quad");
    assert(q && q->type == CFG_ENUM && q->n_allowed == 3);
    assert(q->allowed[0] == 1 && q->allowed[1] == 2 && q->allowed[2] == 4);

    const cfg_field_t *e = find("/recover", "enabled");
    assert(e && e->type == CFG_BOOL && e->def == 1);

    const cfg_field_t *w = find("/recover", "window_ms");
    assert(w && w->min == 1000 && w->max == 10000 && w->def == 5000);

    const cfg_field_t *t = find("/trim", "trim_pct");
    assert(t && t->min == -30 && t->max == 30 && t->def == 0);

    assert(find("/wheel", "nonexistent") == NULL);

    /* Every domain must name a distinct NVS key: two domains sharing one key
       would silently overwrite each other's stored config. */
    for (int i = 0; i < CFG_DOMAIN_COUNT; i++) {
        for (int j = i + 1; j < CFG_DOMAIN_COUNT; j++) {
            assert(strcmp(CFG_DOMAINS[i].nvs_key, CFG_DOMAINS[j].nvs_key) != 0);
        }
    }

    printf("test_cfg_table: OK\n");
    return 0;
}
