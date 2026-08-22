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
    const char *path;
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
    { "/ramp",    ramp_get_v,    ramp_set_v,    ramp_save },
    { "/trim",    trim_get_v,    trim_set_v,    car_save_trim },
    { "/recover", recover_get_v, recover_set_v, recovery_save },
    { "/wheel",   wheel_get_v,   wheel_set_v,   wheel_save },
    { "/dims",    dims_get_v,    dims_set_v,    dims_save },
};

static const cfg_domain_t *domain_for(const char *path) {
    for (int i = 0; i < CFG_DOMAIN_COUNT; i++) {
        if (strcmp(CFG_DOMAINS[i].path, path) == 0) return &CFG_DOMAINS[i];
    }
    return NULL;
}

static const cfg_binding_t *binding_for(const char *path) {
    for (size_t i = 0; i < sizeof(BINDINGS) / sizeof(BINDINGS[0]); i++) {
        if (strcmp(BINDINGS[i].path, path) == 0) return &BINDINGS[i];
    }
    return NULL;
}

static esp_err_t cfg_get(httpd_req_t *req) {
    const cfg_domain_t *d = domain_for(req->uri);
    const cfg_binding_t *b = binding_for(req->uri);
    if (!d || !b) return api_reply_error(req, "404 Not Found", "", "unknown endpoint");

    int32_t vals[CFG_MAX_FIELDS];
    b->get(vals);

    char buf[192];
    int n = snprintf(buf, sizeof(buf), "{");
    for (int i = 0; i < d->n_fields; i++) {
        const cfg_field_t *f = &d->fields[i];
        const char *sep = i ? "," : "";
        int w;
        if (f->type == CFG_BOOL) {
            w = snprintf(buf + n, sizeof(buf) - n, "%s\"%s\":%s",
                         sep, f->name, vals[i] ? "true" : "false");
        } else {
            w = snprintf(buf + n, sizeof(buf) - n, "%s\"%s\":%ld",
                         sep, f->name, (long)vals[i]);
        }
        if (w < 0 || (size_t)(n + w) >= sizeof(buf)) {
            return api_reply_error(req, "500 Internal Server Error", "", "response too long");
        }
        n += w;
    }
    snprintf(buf + n, sizeof(buf) - n, "}");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

static esp_err_t cfg_post(httpd_req_t *req) {
    const cfg_domain_t *d = domain_for(req->uri);
    const cfg_binding_t *b = binding_for(req->uri);
    if (!d || !b) return api_reply_error(req, "404 Not Found", "", "unknown endpoint");

    char body[192];
    if (api_read_body(req, body, sizeof(body)) < 0) {
        return api_reply_error(req, "400 Bad Request", "", "body missing or too long");
    }
    cJSON *j = cJSON_Parse(body);
    if (!j) return api_reply_error(req, "400 Bad Request", "", "malformed JSON");

    int32_t vals[CFG_MAX_FIELDS];
    for (int i = 0; i < d->n_fields; i++) {
        const cfg_field_t *f = &d->fields[i];
        cJSON *it = cJSON_GetObjectItemCaseSensitive(j, f->name);
        if (f->type == CFG_BOOL) {
            if (!cJSON_IsBool(it)) {
                cJSON_Delete(j);
                return api_reply_error(req, "400 Bad Request", f->name, "expected a boolean");
            }
            vals[i] = cJSON_IsTrue(it) ? 1 : 0;
            continue;
        }
        if (!cJSON_IsNumber(it)) {
            cJSON_Delete(j);
            return api_reply_error(req, "400 Bad Request", f->name, "expected a number");
        }
        /* cJSON's valueint TRUNCATES a fractional number, so {"trim_pct":25.7} used to
           apply as 25 under a 200 while the generated validator (the mock, conformance)
           answered 400 "must be an integer" for the same bytes. Same rule both sides. */
        if (it->valuedouble != (double)it->valueint) {
            cJSON_Delete(j);
            return api_reply_error(req, "400 Bad Request", f->name, "not an integer");
        }
        int32_t v = (int32_t)it->valueint;
        if (f->type == CFG_ENUM) {
            bool ok = false;
            for (uint8_t k = 0; k < f->n_allowed; k++) if (f->allowed[k] == v) ok = true;
            if (!ok) {
                cJSON_Delete(j);
                return api_reply_error(req, "400 Bad Request", f->name, "not an allowed value");
            }
        } else if (v < f->min || v > f->max) {
            cJSON_Delete(j);
            return api_reply_error(req, "400 Bad Request", f->name, "out of range");
        }
        vals[i] = v;
    }
    cJSON_Delete(j);

    if (!b->set(vals)) {
        return api_reply_error(req, "500 Internal Server Error", "", "could not apply");
    }
    if (b->save() != ESP_OK) {
        return api_reply_error(req, "500 Internal Server Error", "", "could not persist");
    }
    return api_reply_ok(req);
}

esp_err_t cfg_api_start(void) {
    httpd_handle_t server = http_server_get_handle();
    if (server == NULL) { ESP_LOGE(TAG, "http server not started"); return ESP_FAIL; }
    for (int i = 0; i < CFG_DOMAIN_COUNT; i++) {
        httpd_uri_t g = { .uri = CFG_DOMAINS[i].path, .method = HTTP_GET,  .handler = cfg_get };
        httpd_uri_t p = { .uri = CFG_DOMAINS[i].path, .method = HTTP_POST, .handler = cfg_post };
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &g), TAG, "reg GET %s",
                            CFG_DOMAINS[i].path);
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &p), TAG, "reg POST %s",
                            CFG_DOMAINS[i].path);
    }
    ESP_LOGI(TAG, "config endpoints registered (%d domains)", CFG_DOMAIN_COUNT);
    return ESP_OK;
}
