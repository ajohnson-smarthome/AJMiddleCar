/* The generated table must compile as plain C and carry the schema's numbers. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "cfg_contract.h"
#include "cfg_table.inc"

static const cfg_field_t *find(const char *key, const char *name) {
    for (int i = 0; i < CFG_DOMAIN_COUNT; i++) {
        if (strcmp(CFG_DOMAINS[i].key, key) != 0) continue;
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
    assert(CFG_MAX_FIELDS == 4);
    assert(strcmp(CFG_CONFIG_PATH, "/config") == 0);
    assert(RT_PORT == 4210);
    assert(RT_WATCHDOG_MS == 300);
    assert(RT_MAX_COMMAND == 96);
    assert(RT_MAX_DATAGRAM == 320);
    assert(RT_MAX_COMMAND < RT_MAX_DATAGRAM);
    assert(RT_PROTO == 2);
    for (int i = 0; i < CFG_DOMAIN_COUNT; i++) assert(CFG_DOMAINS[i].n_fields <= CFG_MAX_FIELDS);

    const cfg_field_t *d = find("wheel", "diameter_mm");
    assert(d && d->type == CFG_INT && d->min == 20 && d->max == 150 && d->def == 65 && d->scale == 1);
    const cfg_field_t *g = find("wheel", "gear_ratio");
    assert(g && g->type == CFG_FIXED && g->min == 100 && g->max == 30000 && g->def == 900 && g->scale == 100);
    const cfg_field_t *q = find("wheel", "quadrature");
    assert(q && q->type == CFG_ENUM && q->n_allowed == 3);
    assert(q->allowed[0] == 1 && q->allowed[1] == 2 && q->allowed[2] == 4);
    const cfg_field_t *e = find("recovery", "enabled");
    assert(e && e->type == CFG_BOOL && e->def == 1);
    const cfg_field_t *w = find("recovery", "window_ms");
    assert(w && w->min == 1000 && w->max == 10000 && w->def == 5000);
    const cfg_field_t *t = find("trim", "balance_pct");
    assert(t && t->min == -30 && t->max == 30 && t->def == 0);
    assert(find("wheel", "nonexistent") == NULL);
    assert(find("dims", "track_mm") == NULL);        /* the JSON key is chassis; dims is the NVS key */
    assert(find("chassis", "track_mm") != NULL);

    /* Every domain must name a distinct NVS key and a distinct JSON key. */
    for (int i = 0; i < CFG_DOMAIN_COUNT; i++) {
        for (int j = i + 1; j < CFG_DOMAIN_COUNT; j++) {
            assert(strcmp(CFG_DOMAINS[i].nvs_key, CFG_DOMAINS[j].nvs_key) != 0);
            assert(strcmp(CFG_DOMAINS[i].key, CFG_DOMAINS[j].key) != 0);
        }
    }
    printf("test_cfg_table: OK\n");
    return 0;
}
