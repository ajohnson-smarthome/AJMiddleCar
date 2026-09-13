#include "net_api.h"

#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_log.h"

#include "api_util.h"
#include "wifi_sta.h"

static const char *TAG = "net_api";

/* The network the app told this dongle about, and whether it has yet. In RAM only — nothing
 * about the car's network survives a reboot, on purpose. It used to be persisted in NVS and
 * rejoined at boot before the app arrived, which bought a few seconds on a warm start and
 * cost three things that were worse: a search the app could not tell from its own and
 * would restart from one; credentials that went stale the moment a release changed them,
 * with nothing on the wire to say so (the password is never reported); and a physical
 * reset as the only cure. The app sends the network on every launch, and a dongle that
 * knows nothing until told is a dongle that can never be wrong about it. */
static net_cfg_t s_cfg;
static bool s_configured;

bool net_api_current(net_cfg_t *out)
{
    if (!s_configured) {
        return false;
    }
    *out = s_cfg;
    return true;
}

static esp_err_t net_get(httpd_req_t *req)
{
    char body[128];
    int n = net_cfg_render_public(&s_cfg, s_configured, body, sizeof(body));
    if (n < 0) {
        /* Only reachable if a future field outgrows body — then this is the symptom. */
        ESP_LOGE(TAG, "GET /net does not fit its buffer");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, n);
}

static esp_err_t net_post(httpd_req_t *req)
{
    char raw[256];
    int len = api_read_body(req, raw, sizeof(raw));
    if (len < 0) {
        return api_reply_error(req, "400 Bad Request", "", "body missing or too long");
    }

    /* The one control byte a C string cannot carry. cJSON decodes the JSON escape \u0000
     * into a real NUL inside valuestring, and from there everything downstream — the
     * validator, the copy into wifi_config_t, GET /net's echo — measures the value with
     * strlen and silently keeps only what came before it. That is a truncation, and
     * net_cfg.h's rule for these two fields is "rejected, never clamped": a join aimed at a
     * name the app never sent, with no visible cause, is the outcome the rule exists to
     * prevent. cJSON exposes no length for the decoded string, so the escape is caught in the
     * raw body — and JSON has exactly one spelling for it, so a substring search is exact. */
    if (strstr(raw, "\\u0000") != NULL) {
        return api_reply_error(req, "400 Bad Request", "", "a NUL character is not allowed");
    }

    cJSON *root = cJSON_Parse(raw);
    if (root == NULL) {
        return api_reply_error(req, "400 Bad Request", "", "body is not JSON");
    }

    const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(root, DONGLE_NETKEY_SSID);
    const cJSON *pass = cJSON_GetObjectItemCaseSensitive(root, DONGLE_NETKEY_PASSWORD);
    if (!cJSON_IsString(ssid) || !cJSON_IsString(pass)) {
        cJSON_Delete(root);
        return api_reply_error(req, "400 Bad Request", "",
                               "ssid and password are both required strings");
    }

    net_cfg_t next;
    net_cfg_err_t verr = net_cfg_validate(ssid->valuestring, pass->valuestring, &next);
    cJSON_Delete(root);
    if (verr != NET_CFG_OK) {
        return api_reply_error(req, "400 Bad Request", net_cfg_err_field(verr),
                               net_cfg_err_msg(verr));
    }

    if (s_configured && net_cfg_equal(&s_cfg, &next)) {
        /* Nothing changed. The comparison used to guard a flash write as well; now the radio
         * is the only thing it guards, and that was always the question that mattered. The
         * design says
         * a failed join is reached and HELD rather than retried forever, and that the app
         * decides when to try again by POSTing again — so an unchanged re-POST is precisely
         * how a retry is requested. Returning 200 here without calling the radio meant that
         * once the state reached `failed`, re-POSTing the same credentials did nothing at all
         * and only a power cycle recovered. The rule the plan actually stated is "an unchanged
         * POST must not restart a WORKING radio", and that is what this now enforces.
         *
         * WORKING means connected OR still trying. The first version checked only "connected",
         * and an unchanged POST that landed while the station was on attempt three of five
         * restarted the budget from one — two searches, the second cancelling the first, and
         * «Попытка 1 из 5» on the panel twice over. The case that first produced it (a boot-
         * time join from a stored network, overtaken by the app's own POST) no longer exists,
         * but the case that remains is real: the app relaunching while a search it asked for
         * earlier is still running. A radio mid-search is doing exactly what the POST asks;
         * the only honest answer is 200 and hands off. A rejoin is asked for from `failed`
         * and `idle` alone. */
        if (wifi_sta_connected() || wifi_sta_trying()) {
            return api_reply_ok(req);
        }
        ESP_LOGI(TAG, "network unchanged, but the station is not connected — rejoining %s",
                 s_cfg.ssid);
        if (wifi_sta_join(&s_cfg) != ESP_OK) {
            return api_reply_error(req, "500 Internal Server Error", "",
                                   "the radio refused the join");
        }
        return api_reply_ok(req);
    }

    s_cfg = next;
    s_configured = true;
    ESP_LOGI(TAG, "network set: %s", s_cfg.ssid);
    /* Reported rather than swallowed: before this, every way a join could fail still answered
     * 200, so a client could not tell "joining" from "the radio would not even start". The
     * value is held by this point; a client that retries is retrying the join. */
    if (wifi_sta_join(&s_cfg) != ESP_OK) {
        return api_reply_error(req, "500 Internal Server Error", "",
                               "the radio refused the join");
    }
    return api_reply_ok(req);
}

esp_err_t net_api_register(httpd_handle_t server)
{
    static const httpd_uri_t get_uri = {
        .uri = DONGLE_PATH_NET, .method = HTTP_GET, .handler = net_get,
    };
    static const httpd_uri_t post_uri = {
        .uri = DONGLE_PATH_NET, .method = HTTP_POST, .handler = net_post,
    };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &get_uri), TAG,
                        "cannot register GET /net");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &post_uri), TAG,
                        "cannot register POST /net");
    return ESP_OK;
}
