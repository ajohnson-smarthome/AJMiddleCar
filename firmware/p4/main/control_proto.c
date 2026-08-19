#include "control_proto.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

// Find a JSON number keyed by `key` (e.g. "\"t\"") and parse the value after the
// colon. Zero-alloc; tolerant of whitespace. Anchors the key to a real JSON key
// position (preceded by '{' or ',' after optional whitespace) so a key substring
// inside a string value can't match. Returns 0 on success, -1 otherwise.
static int find_num(const char *msg, const char *key, float *out) {
    const char *p = msg;
    while ((p = strstr(p, key)) != NULL) {
        // Anchor: the char before the opening quote must be '{' or ',' (skipping ws),
        // so we don't match the key inside a string value.
        const char *b = p;
        while (b > msg && (b[-1] == ' ' || b[-1] == '\t' || b[-1] == '\n' || b[-1] == '\r')) b--;
        if (b == msg || b[-1] == '{' || b[-1] == ',') {
            const char *q = p + strlen(key);
            while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
            if (*q == ':') {
                q++;
                char *end;
                float v = strtof(q, &end);
                if (end != q && isfinite(v)) { *out = v; return 0; }
            }
        }
        p += 1;   // keep searching for a properly-anchored occurrence
    }
    return -1;
}

int control_parse_json(const char *msg, float *throttle, float *yaw) {
    if (msg == NULL) return -1;
    float t, y;
    if (find_num(msg, "\"t\"", &t) != 0) return -1;
    if (find_num(msg, "\"y\"", &y) != 0) return -1;
    *throttle = t;
    *yaw = y;
    return 0;
}
