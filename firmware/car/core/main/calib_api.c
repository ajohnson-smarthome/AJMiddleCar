#include "calib_api.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_check.h"
#include "http_server.h"
#include "calibration.h"
#include "calib_wire.h"
#include "calib_spin.h"
#include "car.h"
#include "link.h"
#include "motors.h"
#include "api_util.h"
#include "contract.h"

static const char *TAG = "calib_api";

static esp_err_t reply_table(httpd_req_t *req) {
    motors_config_t cfg;
    car_get_calibration(&cfg);
    char members[320];
    if (calib_table_json(members, sizeof(members), calibration_is_valid(), &cfg) < 0) {
        return api_reply_error(req, "500 Internal Server Error", ERR_INTERNAL, "", "table too long");
    }
    return api_reply_json(req, members);
}

// GET /calibration -> {"proto":2,"calibrated":…,"wheels":[…]}
static esp_err_t calib_get(httpd_req_t *req) { return reply_table(req); }

/* The spin's effects table: the live modules behind calib_spin.h's sequence. */
static bool spin_fx_bus_ok(void *c) { (void)c; return link_bus_ok(); }
static bool spin_fx_spin(void *c, uint8_t pair, bool forward) { (void)c; return car_spin_pair(pair, forward); }
static void spin_fx_hold(void *c) {
    (void)c;
    /* The grant lapses on its own after LINK_HOLD_CALIB_MS, so the pulse ends whether
       or not this handler is still here. The delay is only so the reply lands after
       the wheel has stopped, which is what the wizard's next step assumes. */
    vTaskDelay(pdMS_TO_TICKS(LINK_HOLD_CALIB_MS));
}
static void spin_fx_release(void *c) { (void)c; link_release_must(LINK_SRC_CALIB); }
static const calib_spin_effects_t SPIN_FX = { NULL, spin_fx_bus_ok, spin_fx_spin, spin_fx_hold, spin_fx_release };

// POST /calibration/spin  {"pair":0..3,"direction":"forward"|"reverse"}. Pulses ~0.6 s.
static esp_err_t calib_spin(httpd_req_t *req) {
    char b[96];
    if (api_read_body(req, b, sizeof(b)) < 0) {
        return api_reply_error(req, "400 Bad Request", ERR_BAD_JSON, "", "body missing or too long");
    }
    cJSON *j = cJSON_Parse(b);
    if (!j) return api_reply_error(req, "400 Bad Request", ERR_BAD_JSON, "", "malformed JSON");
    if (!cJSON_IsObject(j)) {
        cJSON_Delete(j);
        return api_reply_error(req, "400 Bad Request", ERR_BAD_JSON, "", "expected a JSON object");
    }
    /* Unknown keys are refused: this is a two-party API where a typo is a bug. */
    for (const cJSON *it = j->child; it; it = it->next) {
        if (strcmp(it->string, KEY_CALIB_PAIR) != 0 && strcmp(it->string, KEY_CALIB_DIRECTION) != 0) {
            esp_err_t e = api_reply_error(req, "400 Bad Request", ERR_UNKNOWN_FIELD, it->string, "no such field");
            cJSON_Delete(j);
            return e;
        }
    }
    cJSON *jp = cJSON_GetObjectItemCaseSensitive(j, KEY_CALIB_PAIR);
    cJSON *jd = cJSON_GetObjectItemCaseSensitive(j, KEY_CALIB_DIRECTION);
    if (!jp) { cJSON_Delete(j); return api_reply_error(req, "400 Bad Request", ERR_MISSING_FIELD, KEY_CALIB_PAIR, "required"); }
    if (!jd) { cJSON_Delete(j); return api_reply_error(req, "400 Bad Request", ERR_MISSING_FIELD, KEY_CALIB_DIRECTION, "required"); }
    if (!cJSON_IsNumber(jp) || jp->valuedouble != (double)jp->valueint) {
        cJSON_Delete(j);
        return api_reply_error(req, "400 Bad Request", ERR_WRONG_TYPE, KEY_CALIB_PAIR, "expected an integer");
    }
    if (!cJSON_IsString(jd)) {
        cJSON_Delete(j);
        return api_reply_error(req, "400 Bad Request", ERR_WRONG_TYPE, KEY_CALIB_DIRECTION, "expected a word");
    }
    int pair = jp->valueint;
    int fwd = calib_direction_forward(jd->valuestring);
    cJSON_Delete(j);
    if (pair < 0 || pair >= CALIB_PAIRS) {
        return api_reply_error(req, "400 Bad Request", ERR_OUT_OF_RANGE, KEY_CALIB_PAIR, "pair 0..3");
    }
    if (fwd < 0) {
        return api_reply_error(req, "400 Bad Request", ERR_NOT_ALLOWED, KEY_CALIB_DIRECTION, "forward or reverse");
    }
    ESP_LOGI(TAG, "spin pair %d %s", pair, fwd ? "fwd" : "rev");
    calib_spin_result_t r = calib_spin_run(&SPIN_FX, (uint8_t)pair, fwd != 0);
    if (r != CALIB_SPIN_DONE) {
        /* 409 is the honest code — the request is fine, the car cannot pulse right now:
           the actuator is taken, or the bus is down and nothing would reach the wheel.
           IDF's httpd_err_code_t has no 409, so the status line is set directly. */
        calib_spin_reply_t no = calib_spin_refusal(r);
        ESP_LOGW(TAG, "spin refused: %s", no.msg);
        return api_reply_error(req, no.status, no.code, "", no.msg);
    }
    return api_reply_ok(req);
}

// POST /calibration  {"wheels":[{"corner","pair","inverted"} x4]} — any order, each corner once.
static esp_err_t calib_save(httpd_req_t *req) {
    // A pretty-printed four-wheel body is 350-460 bytes depending on the indent (212
    // compact); 320 rejected it as "too long". The httpd task's stack is 8 KB now
    // (http_server.c), so 512 here is nothing.
    char b[512];
    if (api_read_body(req, b, sizeof(b)) < 0) {
        return api_reply_error(req, "400 Bad Request", ERR_BAD_JSON, "", "body missing or too long");
    }
    cJSON *j = cJSON_Parse(b);
    if (!j) return api_reply_error(req, "400 Bad Request", ERR_BAD_JSON, "", "malformed JSON");
    if (!cJSON_IsObject(j)) {
        cJSON_Delete(j);
        return api_reply_error(req, "400 Bad Request", ERR_BAD_JSON, "", "expected a JSON object");
    }
    for (const cJSON *it = j->child; it; it = it->next) {
        if (strcmp(it->string, KEY_CALIB_WHEELS) != 0) {
            esp_err_t e = api_reply_error(req, "400 Bad Request", ERR_UNKNOWN_FIELD, it->string, "no such field");
            cJSON_Delete(j);
            return e;
        }
    }
    cJSON *arr = cJSON_GetObjectItemCaseSensitive(j, KEY_CALIB_WHEELS);
    if (!arr) { cJSON_Delete(j); return api_reply_error(req, "400 Bad Request", ERR_MISSING_FIELD, KEY_CALIB_WHEELS, "required"); }
    if (!cJSON_IsArray(arr) || cJSON_GetArraySize(arr) != POS_COUNT) {
        cJSON_Delete(j);
        return api_reply_error(req, "400 Bad Request", ERR_WRONG_TYPE, KEY_CALIB_WHEELS, "expected four wheels");
    }
    motors_config_t cfg = { .deadzone = 0.05f };
    unsigned seen = 0;
    char where[40];
    for (int i = 0; i < POS_COUNT; i++) {
        cJSON *w = cJSON_GetArrayItem(arr, i);
        cJSON *jc = cJSON_GetObjectItemCaseSensitive(w, KEY_CALIB_CORNER);
        cJSON *jp = cJSON_GetObjectItemCaseSensitive(w, KEY_CALIB_PAIR);
        cJSON *ji = cJSON_GetObjectItemCaseSensitive(w, KEY_CALIB_INVERTED);
        snprintf(where, sizeof(where), "%s[%d]", KEY_CALIB_WHEELS, i);
        for (const cJSON *it = cJSON_IsObject(w) ? w->child : NULL; it; it = it->next) {
            if (strcmp(it->string, KEY_CALIB_CORNER) != 0 && strcmp(it->string, KEY_CALIB_PAIR) != 0 &&
                strcmp(it->string, KEY_CALIB_INVERTED) != 0) {
                esp_err_t e = api_reply_error(req, "400 Bad Request", ERR_UNKNOWN_FIELD, where, "no such field");
                cJSON_Delete(j);
                return e;
            }
        }
        if (!cJSON_IsString(jc) || !cJSON_IsNumber(jp) || !cJSON_IsBool(ji) ||
            jp->valuedouble != (double)jp->valueint) {
            cJSON_Delete(j);
            return api_reply_error(req, "400 Bad Request", ERR_WRONG_TYPE, where, "wheel needs {corner,pair,inverted}");
        }
        int pos = calib_corner_index(jc->valuestring);
        if (pos < 0) {
            cJSON_Delete(j);
            return api_reply_error(req, "400 Bad Request", ERR_NOT_ALLOWED, where, "unknown corner");
        }
        if (seen & (1u << pos)) {
            cJSON_Delete(j);
            return api_reply_error(req, "400 Bad Request", ERR_NOT_ALLOWED, where, "corner repeated");
        }
        seen |= 1u << pos;
        /* Range-checked BEFORE narrowing: (uint8_t)256 is 0, inside what calibration_valid accepts. */
        if (jp->valueint < 0 || jp->valueint >= CALIB_PAIRS) {
            cJSON_Delete(j);
            return api_reply_error(req, "400 Bad Request", ERR_OUT_OF_RANGE, where, "pair 0..3");
        }
        cfg.wheels[pos].channel_pair = (uint8_t)jp->valueint;
        cfg.wheels[pos].sign = cJSON_IsTrue(ji) ? -1 : 1;
    }
    cJSON_Delete(j);
    esp_err_t e = calibration_save(&cfg);   /* validates: pairs 0..3 each once */
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "save rejected: %s", esp_err_to_name(e));
        /* ESP_ERR_INVALID_ARG is the validator's own answer — the client's fault. Any
           other code is a flash/NVS failure: the request was fine, persisting it was not. */
        if (e == ESP_ERR_INVALID_ARG) {
            return api_reply_error(req, "400 Bad Request", ERR_NOT_ALLOWED, KEY_CALIB_WHEELS, "pairs must be 0..3, each once");
        }
        return api_reply_error(req, "500 Internal Server Error", ERR_WRITE_FAILED, KEY_CALIB_WHEELS, "could not persist");
    }
    car_set_calibration(&cfg);
    calibration_set_valid(true);
    ESP_LOGI(TAG, "calibration saved and applied");
    return reply_table(req);
}

esp_err_t calib_api_start(void) {
    httpd_handle_t server = http_server_get_handle();
    if (server == NULL) { ESP_LOGE(TAG, "http server not started"); return ESP_FAIL; }
    httpd_uri_t get  = { .uri = PATH_CALIBRATION, .method = HTTP_GET,  .handler = calib_get };
    httpd_uri_t spin = { .uri = PATH_SPIN,        .method = HTTP_POST, .handler = calib_spin };
    httpd_uri_t save = { .uri = PATH_CALIBRATION, .method = HTTP_POST, .handler = calib_save };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &get),  TAG, "reg GET " PATH_CALIBRATION);
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &spin), TAG, "reg POST " PATH_SPIN);
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &save), TAG, "reg POST " PATH_CALIBRATION);
    ESP_LOGI(TAG, "calibration endpoints registered");
    return ESP_OK;
}
