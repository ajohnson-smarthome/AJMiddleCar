#include "cfg_api.h"
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_check.h"
#include "http_server.h"
#include "cfg_contract.h"
#include "contract.h"
#include "cfg_value.h"
#include "ramp.h"
#include "car.h"
#include "recovery.h"
#include "wheel.h"
#include "dims.h"
#include "api_util.h"

static const char *TAG = "cfg_api";

_Static_assert(CFG_MAX_FIELDS <= 8, "widen vals[] below");

/* Values move as an array of int32 in the field order the generated table declares, so
   the generic handler never needs to know a domain's struct layout. The binding is the
   one part of a config domain a schema cannot describe. */
typedef void      (*cfg_get_fn)(int32_t *out);
typedef bool      (*cfg_set_fn)(const int32_t *in);
typedef esp_err_t (*cfg_save_fn)(void);

typedef struct {
    const char *key;
    cfg_get_fn  get;
    cfg_set_fn  set;
    cfg_save_fn save;
} cfg_binding_t;

static void ramp_get_v(int32_t *o) { o[0] = ramp_get_ms(); }
static bool ramp_set_v(const int32_t *v) { return ramp_set_ms((uint16_t)v[0]); }

static void trim_get_v(int32_t *o) { o[0] = car_get_trim(); }
static bool trim_set_v(const int32_t *v) { car_set_trim((int8_t)v[0]); return true; }

static void recover_get_v(int32_t *o) {
    bool en; uint16_t win;
    recovery_get_config(&en, &win);
    o[0] = en ? 1 : 0; o[1] = win;
}
static bool recover_set_v(const int32_t *v) {
    recovery_set_config(v[0] != 0, (uint16_t)v[1]);
    return true;
}

static void wheel_get_v(int32_t *o) {
    wheel_params_t w; wheel_get(&w);
    o[0] = w.diameter_mm; o[1] = w.ppr; o[2] = w.gear_x100; o[3] = w.quad;
}
static bool wheel_set_v(const int32_t *v) {
    wheel_params_t w = { (uint16_t)v[0], (uint16_t)v[1], (uint16_t)v[2], (uint8_t)v[3] };
    wheel_set(&w);
    return true;
}

static void dims_get_v(int32_t *o) {
    dims_params_t d; dims_get(&d);
    o[0] = d.track_mm; o[1] = d.wheelbase_mm;
}
static bool dims_set_v(const int32_t *v) {
    dims_params_t d = { (uint16_t)v[0], (uint16_t)v[1] };
    dims_set(&d);
    return true;
}

static const cfg_binding_t BINDINGS[] = {
    { "ramp",     ramp_get_v,    ramp_set_v,    ramp_save },
    { "trim",     trim_get_v,    trim_set_v,    car_save_trim },
    { "recovery", recover_get_v, recover_set_v, recovery_save },
    { "wheel",    wheel_get_v,   wheel_set_v,   wheel_save },
    { "chassis",  dims_get_v,    dims_set_v,    dims_save },
};

static const cfg_binding_t *binding_for(const char *key) {
    for (size_t i = 0; i < sizeof(BINDINGS) / sizeof(BINDINGS[0]); i++) {
        if (strcmp(BINDINGS[i].key, key) == 0) return &BINDINGS[i];
    }
    return NULL;
}

/* One domain as "<key>":{...}, appended at buf+*at. */
static bool render_domain(const cfg_domain_t *d, const int32_t *vals, char *buf, size_t n, size_t *at) {
    int w = snprintf(buf + *at, n - *at, "\"%s\":{", d->key);
    if (w < 0 || (size_t)w >= n - *at) return false;
    *at += (size_t)w;
    for (int i = 0; i < d->n_fields; i++) {
        char v[32];
        if (cfg_value_print(&d->fields[i], vals[i], v, sizeof(v)) < 0) return false;
        w = snprintf(buf + *at, n - *at, "%s\"%s\":%s", i ? "," : "", d->fields[i].name, v);
        if (w < 0 || (size_t)w >= n - *at) return false;
        *at += (size_t)w;
    }
    if (*at + 2 > n) return false;
    buf[(*at)++] = '}';
    buf[*at] = '\0';
    return true;
}

/* Every domain, as the members of /config's body. The reply to a POST is this too: the
   client sees what the car now holds, not a receipt. */
static esp_err_t reply_config(httpd_req_t *req) {
    char members[400];
    size_t at = 0;
    for (int i = 0; i < CFG_DOMAIN_COUNT; i++) {
        const cfg_domain_t *d = &CFG_DOMAINS[i];
        const cfg_binding_t *b = binding_for(d->key);
        if (!b) return api_reply_error(req, "500 Internal Server Error", ERR_INTERNAL, "", "domain unbound");
        int32_t vals[CFG_MAX_FIELDS];
        b->get(vals);
        if (i && at + 1 < sizeof(members)) members[at++] = ',';
        if (!render_domain(d, vals, members, sizeof(members), &at)) {
            return api_reply_error(req, "500 Internal Server Error", ERR_INTERNAL, "", "response too long");
        }
    }
    return api_reply_json(req, members);
}

static esp_err_t cfg_get(httpd_req_t *req) { return reply_config(req); }

/* A dotted path for the error's field member. */
static const char *dotted(char *buf, size_t n, const char *key, const char *name) {
    snprintf(buf, n, "%s.%s", key, name);
    return buf;
}

/* Parse one present domain into vals[]. Returns NULL, or an error already sent. */
static esp_err_t parse_domain(httpd_req_t *req, const cfg_domain_t *d, const cJSON *obj,
                              int32_t *vals, bool *sent) {
    char where[48];
    *sent = false;
    if (!cJSON_IsObject(obj)) {
        *sent = true;
        return api_reply_error(req, "400 Bad Request", ERR_WRONG_TYPE, d->key, "expected an object");
    }
    /* Unknown keys are refused: this is a two-party API where a typo is a bug. */
    for (const cJSON *it = obj->child; it; it = it->next) {
        bool known = false;
        for (int i = 0; i < d->n_fields; i++) if (strcmp(d->fields[i].name, it->string) == 0) known = true;
        if (!known) {
            *sent = true;
            return api_reply_error(req, "400 Bad Request", ERR_UNKNOWN_FIELD,
                                   dotted(where, sizeof(where), d->key, it->string), "no such field");
        }
    }
    for (int i = 0; i < d->n_fields; i++) {
        const cfg_field_t *f = &d->fields[i];
        const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, f->name);
        dotted(where, sizeof(where), d->key, f->name);
        if (!it) {
            *sent = true;
            return api_reply_error(req, "400 Bad Request", ERR_MISSING_FIELD, where, "required");
        }
        if (f->type == CFG_BOOL) {
            if (!cJSON_IsBool(it)) {
                *sent = true;
                return api_reply_error(req, "400 Bad Request", ERR_WRONG_TYPE, where, "expected a boolean");
            }
            vals[i] = cJSON_IsTrue(it) ? 1 : 0;
            continue;
        }
        if (!cJSON_IsNumber(it) || !cfg_value_from_double(f, it->valuedouble, &vals[i])) {
            *sent = true;
            return api_reply_error(req, "400 Bad Request", ERR_WRONG_TYPE, where,
                                   f->type == CFG_FIXED ? "expected a number" : "expected an integer");
        }
        const char *bad = cfg_value_check(f, vals[i]);
        if (bad) {
            *sent = true;
            return api_reply_error(req, "400 Bad Request", bad, where,
                                   strcmp(bad, ERR_NOT_ALLOWED) == 0 ? "not an allowed value" : "out of range");
        }
    }
    return ESP_OK;
}

static esp_err_t cfg_post(httpd_req_t *req) {
    char body[512];
    if (api_read_body(req, body, sizeof(body)) < 0) {
        return api_reply_error(req, "400 Bad Request", ERR_BAD_JSON, "", "body missing or too long");
    }
    cJSON *j = cJSON_Parse(body);
    if (!j) return api_reply_error(req, "400 Bad Request", ERR_BAD_JSON, "", "malformed JSON");
    if (!cJSON_IsObject(j)) {
        cJSON_Delete(j);
        return api_reply_error(req, "400 Bad Request", ERR_BAD_JSON, "", "expected a JSON object");
    }
    /* Pass one: validate everything. Nothing is applied until every present domain is
       good, so a body that is half right changes nothing. */
    bool present[CFG_DOMAIN_COUNT] = {0};
    int32_t vals[CFG_DOMAIN_COUNT][CFG_MAX_FIELDS];
    int n_present = 0;
    for (const cJSON *it = j->child; it; it = it->next) {
        int which = -1;
        for (int i = 0; i < CFG_DOMAIN_COUNT; i++) if (strcmp(CFG_DOMAINS[i].key, it->string) == 0) which = i;
        if (which < 0) {
            esp_err_t e = api_reply_error(req, "400 Bad Request", ERR_UNKNOWN_FIELD, it->string,
                                          "not a configuration domain");
            cJSON_Delete(j);
            return e;
        }
        bool sent;
        esp_err_t e = parse_domain(req, &CFG_DOMAINS[which], it, vals[which], &sent);
        if (sent) { cJSON_Delete(j); return e; }
        present[which] = true;
        n_present++;
    }
    cJSON_Delete(j);
    if (n_present == 0) {
        return api_reply_error(req, "400 Bad Request", ERR_MISSING_FIELD, "", "no configuration domain in the body");
    }
    /* Pass two: apply, then persist, domain by domain. A persist that fails rolls its own
       domain back before answering — the running car and the client must not hold two
       truths — and answers 500; domains applied before it stay applied, which the reply
       (had it been sent) would have shown. */
    for (int i = 0; i < CFG_DOMAIN_COUNT; i++) {
        if (!present[i]) continue;
        const cfg_binding_t *b = binding_for(CFG_DOMAINS[i].key);
        if (!b) return api_reply_error(req, "500 Internal Server Error", ERR_INTERNAL, CFG_DOMAINS[i].key, "domain unbound");
        int32_t prev[CFG_MAX_FIELDS];
        b->get(prev);
        if (!b->set(vals[i])) {
            return api_reply_error(req, "500 Internal Server Error", ERR_WRITE_FAILED, CFG_DOMAINS[i].key, "could not apply");
        }
        if (b->save() != ESP_OK) {
            if (!b->set(prev)) {
                ESP_LOGE(TAG, "%s: could not persist, and could not roll back either — the running "
                              "value now differs from both NVS and the client", CFG_DOMAINS[i].key);
            }
            return api_reply_error(req, "500 Internal Server Error", ERR_WRITE_FAILED, CFG_DOMAINS[i].key, "could not persist");
        }
    }
    return reply_config(req);
}

esp_err_t cfg_api_start(void) {
    httpd_handle_t server = http_server_get_handle();
    if (server == NULL) { ESP_LOGE(TAG, "http server not started"); return ESP_FAIL; }
    httpd_uri_t g = { .uri = CFG_CONFIG_PATH, .method = HTTP_GET,  .handler = cfg_get };
    httpd_uri_t p = { .uri = CFG_CONFIG_PATH, .method = HTTP_POST, .handler = cfg_post };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &g), TAG, "reg GET " CFG_CONFIG_PATH);
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &p), TAG, "reg POST " CFG_CONFIG_PATH);
    ESP_LOGI(TAG, "config endpoint registered (%d domains)", CFG_DOMAIN_COUNT);
    return ESP_OK;
}
