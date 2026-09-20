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

/* Does `frame` contain `"name":` inside the object that begins right after `"group":{`? */
static int in_group(const char *frame, const char *group, const char *name) {
    char open[80], key[80];
    snprintf(open, sizeof(open), "\"%s\":{", group);
    snprintf(key, sizeof(key), "\"%s\":", name);
    const char *g = strstr(frame, open);
    if (!g) return 0;
    const char *end = strchr(g, '}');
    const char *k = strstr(g, key);
    return k != NULL && end != NULL && k < end;
}

/* The next quoted string at or after `*p`, bounded by `end`. Advances `*p` past its
   closing quote on success, so a run of neighbouring quoted values ("a", "b", "c")
   walks forward correctly no matter how many have already been consumed — unlike a
   walk that leaves the cursor sitting ON the previous closing quote, which resyncs
   one quote short of where the next value actually starts. */
static int next_quoted(const char **p, const char *end, char *out, size_t cap) {
    const char *open = strchr(*p, '"');
    if (!open || open > end) return 0;
    const char *close = strchr(open + 1, '"');
    if (!close || close > end) return 0;
    size_t len = (size_t)(close - open - 1);
    if (len >= cap) return 0;
    memcpy(out, open + 1, len);
    out[len] = '\0';
    *p = close + 1;
    return 1;
}

/* The "]" that matches the "[" at `open`, by depth counting. A naive "first ']' after
   here" landed inside a field's own nested array instead — motors.bus carries
   "values": ["ok", "down"], whose closing bracket sits well before the fields array's
   real end, so it silently cut "calibrated" and "owner" out of the walk. Schema strings
   carry no escapes (the header comment's own assumption), so brackets inside a quoted
   string are not a concern here. */
static const char *matching_bracket(const char *open) {
    int depth = 0;
    for (const char *p = open; *p; p++) {
        if (*p == '[') depth++;
        else if (*p == ']') { if (--depth == 0) return p; }
    }
    return NULL;
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

    /* --- telemetry: every field of every group the schema lists, in its group ---- */
    telemetry_t t = { .seq = 88, .rssi = -55, .rx_hz = 10, .timeouts = 2, .uptime_s = 123,
                      .free_heap = 198000, .calibrated = true, .owner = MOTORS_OWNER_REMOTE,
                      .bus_ok = true, .battery_state = BATTERY_STATE_OK, .battery_mv = 12310,
                      .battery_ma = 3100, .battery_mw = 38200, .battery_soc = 72 };
    char frame[RT_MAX_DATAGRAM];
    assert(telemetry_datagram(frame, sizeof(frame), &t) > 0);

    /* Walk "telemetry".groups: each quoted name there is a group; that group's own
       definition lives under the top-level "groups" object, and every "name" inside its
       "fields" array must sit inside that group's object in the printed frame.

       "telemetry" is anchored on `"telemetry": {` rather than bare `"telemetry"`: the rt
       section's type table carries a `"telemetry": "telemetry"` entry earlier in the
       file, and a bare match lands there instead of on telemetry's own group. */
    const char *groups_def = strstr(json, "\"groups\"");    /* the top-level group defs */
    assert(groups_def);
    const char *telemetry = strstr(json, "\"telemetry\": {");
    if (!telemetry) telemetry = strstr(json, "\"telemetry\":{");
    assert(telemetry);
    const char *tg = strstr(telemetry, "\"groups\"");        /* telemetry's own groups list */
    assert(tg);
    const char *tg_open = strchr(tg, '[');
    const char *tg_end  = strchr(tg, ']');
    assert(tg_open && tg_end && tg_open < tg_end);

    int n_fields = 0, n_groups = 0;
    const char *p = tg_open;
    char gname[32];
    while (next_quoted(&p, tg_end, gname, sizeof(gname))) {
        n_groups++;
        char pat[48];
        snprintf(pat, sizeof(pat), "\"%s\": {", gname);
        const char *def = strstr(groups_def, pat);
        if (!def) { snprintf(pat, sizeof(pat), "\"%s\":{", gname); def = strstr(groups_def, pat); }
        assert(def);
        const char *fields_key = strstr(def, "\"fields\"");
        assert(fields_key);
        const char *fields_open = strchr(fields_key, '[');
        assert(fields_open);
        const char *def_end = matching_bracket(fields_open);   /* the fields array's end */
        assert(def_end);
        const char *fs = def;
        for (;;) {
            const char *next = str_after(fs, "name", v, sizeof(v));
            if (!next || next > def_end) break;
            fs = next;
            if (!in_group(frame, gname, v)) {
                printf("FAIL telemetry field \"%s.%s\" is in the schema and not in the frame:\n"
                       "  %s\n", gname, v, frame);
                assert(0);
            }
            n_fields++;
        }
    }
    assert(n_groups == 5);
    assert(n_fields == 17);
    assert(strstr(frame, "\"proto\":2,\"type\":\"telemetry\",\"seq\":88,"));

    /* --- null where the schema allows it, and only there ------------------------- */
    /* The battery group's numbers are nullable (car/battery-monitor: `absent`, or the
       remainder not yet determined); TELEMETRY_NULL is the printer's "no value". The key
       must still be inside the group, spelled `null` — a field that vanished instead
       would be a missing key to the app's decoder, not a nil. */
    t.battery_state = BATTERY_STATE_ABSENT;
    t.battery_mv = t.battery_ma = t.battery_mw = t.battery_soc = INT32_MIN;
    assert(telemetry_datagram(frame, sizeof(frame), &t) > 0);
    assert(in_group(frame, KEY_GROUP_BATTERY, KEY_BATTERY_VOLTAGE_MV));
    assert(strstr(frame, "\"" KEY_GROUP_BATTERY "\":{\"" KEY_BATTERY_VOLTAGE_MV "\":null,\""
                         KEY_BATTERY_CURRENT_MA "\":null,\"" KEY_BATTERY_POWER_MW "\":null,\""
                         KEY_BATTERY_SOC_PCT "\":null,\"" KEY_BATTERY_STATE "\":\"" BATTERY_STATE_ABSENT "\"}"));
    /* Only soc_pct undetermined: the three measurements print, the remainder is null. */
    t.battery_state = BATTERY_STATE_OK;
    t.battery_mv = 12600; t.battery_ma = 120; t.battery_mw = 1512; t.battery_soc = INT32_MIN;
    assert(telemetry_datagram(frame, sizeof(frame), &t) > 0);
    assert(strstr(frame, "\"" KEY_BATTERY_POWER_MW "\":1512,\"" KEY_BATTERY_SOC_PCT "\":null,\""
                         KEY_BATTERY_STATE "\":\"" BATTERY_STATE_OK "\"}"));
    /* The charge direction is a sign on the wire, not a word. */
    t.battery_ma = -850;
    assert(telemetry_datagram(frame, sizeof(frame), &t) > 0);
    assert(strstr(frame, "\"" KEY_BATTERY_CURRENT_MA "\":-850,"));

    free(json);
    printf("test_contract_wire: OK (%d telemetry fields in %d groups, device \"%s\")\n",
           n_fields, n_groups, CAR_DEVICE_ID);
    return 0;
}
