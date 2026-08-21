#ifndef CFG_CONTRACT_H
#define CFG_CONTRACT_H

#include <stdint.h>

/* Types for the generated config descriptor table (cfg_table.inc).
   This header is hand-written; the table that uses it is not. */

typedef enum { CFG_INT, CFG_BOOL, CFG_ENUM } cfg_type_t;

typedef struct {
    const char    *name;
    cfg_type_t     type;
    int32_t        min;         /* for CFG_BOOL: 0..1; for CFG_ENUM: the value bounds */
    int32_t        max;
    int32_t        def;
    const int32_t *allowed;     /* NULL unless CFG_ENUM */
    uint8_t        n_allowed;
} cfg_field_t;

typedef struct {
    const char        *path;
    const char        *nvs_key;
    const cfg_field_t *fields;
    uint8_t            n_fields;
} cfg_domain_t;

#endif /* CFG_CONTRACT_H */
