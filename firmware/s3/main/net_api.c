#include "net_api.h"

#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"

#include "api_util.h"
#include "wifi_sta.h"

static const char *TAG = "net_api";

/* One JSON string under one key, which is how the car stores each of its five config
 * domains. The namespace is the dongle's own — nothing here shares storage with anything. */
static const char NVS_NAMESPACE[] = "dongle";
static const char NVS_KEY[] = "net";

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

/* Persist, unless the stored bytes already say this. The dirty check is the point: an app
 * that POSTs its configuration unconditionally at every launch — which is exactly what the
 * app does — must not erase a flash sector each time. */
static esp_err_t store(const net_cfg_t *cfg)
{
    /* 216 worst case: 25 literal + 32×2 escaped SSID + 63×2 escaped password + NUL; see
     * net_cfg_validate's comment in net_cfg.h for why that ×2 is provable rather than a
     * guess. */
    char json[256];
    if (net_cfg_render_stored(cfg, json, sizeof(json)) < 0) {
        return ESP_FAIL;
    }

    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h), TAG, "cannot open nvs");

    char existing[256];
    size_t len = sizeof(existing);
    if (nvs_get_str(h, NVS_KEY, existing, &len) == ESP_OK && strcmp(existing, json) == 0) {
        nvs_close(h);
        return ESP_OK;
    }

    esp_err_t err = nvs_set_str(h, NVS_KEY, json);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

void net_api_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;                      /* nothing stored yet — the first boot */
    }

    /* Matches store()'s 216-byte worst case above. A short buffer here would fail to read
     * back exactly the maximal configs store() can legitimately write, and silently — the
     * ESP_ERR_NVS_INVALID_LENGTH falls into the same "nothing stored" return below. */
    char json[256];
    size_t len = sizeof(json);
    esp_err_t err = nvs_get_str(h, NVS_KEY, json, &len);
    nvs_close(h);
    if (err != ESP_OK) {
        return;
    }

    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        ESP_LOGW(TAG, "stored config is not JSON; ignoring it");
        return;
    }
    const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(root, DONGLE_NETKEY_SSID);
    const cJSON *pass = cJSON_GetObjectItemCaseSensitive(root, DONGLE_NETKEY_PASSWORD);
    if (cJSON_IsString(ssid) && cJSON_IsString(pass) &&
        net_cfg_validate(ssid->valuestring, pass->valuestring, &s_cfg) == NET_CFG_OK) {
        s_configured = true;
        ESP_LOGI(TAG, "network configured: %s", s_cfg.ssid);
    } else {
        /* Revalidated rather than trusted: bounds can move between firmware versions, and
         * a stored value that no longer passes must not reach the radio. */
        ESP_LOGW(TAG, "stored config failed validation; ignoring it");
    }
    cJSON_Delete(root);
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
        return api_reply_ok(req);    /* nothing changed: no flash write, no radio churn */
    }

    if (store(&next) != ESP_OK) {
        return api_reply_error(req, "500 Internal Server Error", "", "cannot persist");
    }
    s_cfg = next;
    s_configured = true;
    ESP_LOGI(TAG, "network set: %s", s_cfg.ssid);
    wifi_sta_join(&s_cfg);   /* only here: the value actually changed and NVS now has it */
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
