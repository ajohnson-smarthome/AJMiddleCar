#ifndef DIMS_H
#define DIMS_H

#include <stdint.h>

// Distances between wheel centres (mm). Validated by dims_set + the /dims API.
#define DIMS_TRACK_MIN_MM       60
#define DIMS_TRACK_MAX_MM       300
#define DIMS_WHEELBASE_MIN_MM   90
#define DIMS_WHEELBASE_MAX_MM   360

// track_mm = lateral (left↔right wheel centres); wheelbase_mm = longitudinal (front↔rear).
typedef struct {
    uint16_t track_mm;
    uint16_t wheelbase_mm;
} dims_params_t;

// Load from NVS (or defaults: track 130, wheelbase 210). Call once at boot.
void dims_init(void);
// Copy current params out.
void dims_get(dims_params_t *out);
// Validate/clamp and store in RAM (the /dims API persists to NVS).
void dims_set(const dims_params_t *in);
// Serialize the current dims to a JSON string and persist to NVS (one key).
void dims_save(void);

#endif // DIMS_H
