/* The values the wire carries that the C generator does not emit a symbol for, pinned
 * to contract/car-api.json by reading it.
 *
 * The generator gives C the rt frame's key names, the caps and the ctl vocabulary, so
 * those are used directly and cannot drift. Two things are left over: the device
 * identity (hand-written in identity.h, because it is also the SSID and the softAP
 * password) and the telemetry field names (a format string in telemetry.h). Both are
 * read by the app and the mock from the schema. Renaming one there and not here used
 * to regenerate cleanly, pass every test, and present on the phone as "wrong car" or
 * as a field that silently stopped arriving — so this test does the comparing.
 */
#define TELEMETRY_HOST_TEST
#include "../main/telemetry.h"
#include "../main/identity.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef CONTRACT_JSON
#error "CONTRACT_JSON must name contract/car-api.json — see the Makefile"
#endif

static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { printf("FAIL cannot open %s\n", path); assert(0); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    assert(buf && fread(buf, 1, (size_t)n, f) == (size_t)n);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

/* The value of the first `"key": "..."` at or after `from`, copied into `out`. Enough
   JSON for a schema whose shape is fixed and whose strings carry no escapes. */
static const char *str_after(const char *from, const char *key, char *out, size_t cap) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(from, pat);
    if (!p) return NULL;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != ':') return NULL;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != '"') return NULL;
    p++;
    const char *end = strchr(p, '"');
    if (!end || (size_t)(end - p) >= cap) return NULL;
    memcpy(out, p, (size_t)(end - p));
    out[end - p] = '\0';
    return end + 1;
}

static void same(const char *what, const char *schema, const char *firmware) {
    if (strcmp(schema, firmware) != 0) {
        printf("FAIL %s: schema says \"%s\", the firmware says \"%s\"\n",
               what, schema, firmware);
        assert(0);
    }
}

int main(void) {
    char *json = slurp(CONTRACT_JSON);
    char v[64];

    /* --- identity: one car, one set of names -------------------------------- */
    assert(str_after(json, "device", v, sizeof(v)));
    same("device", v, CAR_DEVICE_ID);
    assert(str_after(json, "ssid", v, sizeof(v)));
    same("network.ssid", v, CAR_AP_SSID);
    assert(str_after(json, "password", v, sizeof(v)));
    same("network.password", v, CAR_AP_PASS);

    /* --- telemetry: the same names, in the same order ------------------------ */
    const char *sect = strstr(json, "\"telemetry\"");
    assert(sect);
    const char *end = strstr(sect, "\"ctl_values\"");
    assert(end);

    telemetry_t t = { .seq = 88, .rssi = -55, .rx_fps = 10, .wdt_trips = 2,
                      .uptime_s = 123, .heap = 198000, .calibrated = true,
                      .ctl = "rt", .bus_ok = true };
    char frame[224];
    assert(telemetry_fields(frame, sizeof(frame), &t) > 0);

    int n_fields = 0;
    const char *scan = sect;
    size_t at = 0;   /* how far into the frame the names have been matched, in order */
    for (;;) {
        const char *next = str_after(scan, "name", v, sizeof(v));
        if (!next || next > end) break;
        scan = next;
        char want[80];
        snprintf(want, sizeof(want), "\"%s\":", v);
        const char *found = strstr(frame + at, want);
        if (!found) {
            printf("FAIL telemetry field \"%s\" is in the schema and not in the frame:\n"
                   "  %s\n", v, frame);
            assert(0);
        }
        at = (size_t)(found - frame) + strlen(want);   /* forward only: order matters */
        n_fields++;
    }
    assert(n_fields == 9);

    /* Nothing extra either: one comma between neighbours, none inside these values. */
    int commas = 0;
    for (const char *p = frame; *p; p++) if (*p == ',') commas++;
    if (commas + 1 != n_fields) {
        printf("FAIL the frame carries %d fields, the schema names %d:\n  %s\n",
               commas + 1, n_fields, frame);
        assert(0);
    }

    free(json);
    printf("test_contract_wire: OK (%d telemetry fields, device \"%s\")\n",
           n_fields, CAR_DEVICE_ID);
    return 0;
}
