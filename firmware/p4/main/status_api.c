#include "status_api.h"
#include <stdio.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_app_desc.h"
#include "http_server.h"
#include "telemetry.h"
#include "identity.h"
#include "board.h"
#include "contract.h"
#include <string.h>
#include "esp_ota_ops.h"
#include "api_util.h"
#include "eh_common_fw_version.h"   /* esp_hosted's own version macros — the single source */
#include "radio_flash.h"
#include "radio_expected.h"

static const char *TAG = "status_api";

// The radio co-processor's firmware version, read once at boot. Reading it per request would put
// SDIO traffic on the app's 1.5 s status poll for a value that cannot change without a reboot.
// It matters because the C6's image is delivered over SDIO from the host and its expected version
// derives from the esp_hosted component pin: a mismatch has no other symptom than the radio
// misbehaving in ways that look like anything else.
//
// Written once by read_radio_version() — which status_api_start runs BEFORE registering
// the handler — then only read, so the cross-task safety is ordering, not a lock.
static char s_radio_fw[24] = "unavailable";
static bool s_radio_ok     = false;

/* Did the bootloader revert the previous OTA? The other slot is left ESP_OTA_IMG_ABORTED
 * exactly when an update failed its first boot — the one signal a client has that the
 * image it flashed did not survive. Read once at start: the answer cannot change without
 * a reboot. */
static bool s_rollback  = false;
static bool s_nvs_wiped = false;

void status_api_note_nvs_wiped(void) { s_nvs_wiped = true; }

static void read_rollback_state(void) {
    const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
    esp_ota_img_states_t st;
    s_rollback = other != NULL &&
                 esp_ota_get_state_partition(other, &st) == ESP_OK &&
                 st == ESP_OTA_IMG_ABORTED;
    if (s_rollback) ESP_LOGW(TAG, "the previous OTA was rolled back by the bootloader");
}

static void read_radio_version(void) {
    snprintf(s_radio_fw, sizeof(s_radio_fw), "%s", radio_flash_version());
    s_radio_ok = (strcmp(s_radio_fw, RADIO_EXPECTED_FW) == 0);
    if (s_radio_ok) {
        ESP_LOGI(TAG, "radio firmware %s", s_radio_fw);
    } else {
        /* No longer an instruction to fetch a cable: main.c's boot gate offers the embedded
         * image first, and only a spent attempt budget or a build with no image reaches here
         * still mismatched. */
        ESP_LOGW(TAG, "radio firmware %s, expected %s — the car could not correct it",
                 s_radio_fw, RADIO_EXPECTED_FW);
    }
}

static esp_err_t status_get(httpd_req_t *req) {
    telemetry_t t;
    telemetry_gather(&t, TELEM_STATUS);
    char fields[224];
    if (telemetry_fields(fields, sizeof(fields), &t) < 0) {
        /* The same envelope as the overflow path below, and as every other endpoint:
           a client that parses errors as JSON must not meet plain text on one branch
           of one handler. */
        ESP_LOGE(TAG, "/status could not render its telemetry fields");
        return api_reply_error(req, "500 Internal Server Error", "", "telemetry unavailable");
    }
    const char *fw = esp_app_get_description()->version;
    char buf[480];
    /* The same three keys the hello reply carries, spelled from the same schema: one
       wire value must not have two spellings, or a rename presents as "wrong car". */
    int n = snprintf(buf, sizeof(buf),
                     "{\"" RT_KEY_DEVICE "\":\"" CAR_DEVICE_ID "\",\"" RT_KEY_FW "\":\"%s\","
                     "\"" RT_KEY_PROTO "\":%d,%s,"
                     "\"rollback\":%s,\"nvs_wiped\":%s,"
                     "\"radio\":{\"" RT_KEY_FW "\":\"%s\",\"expected\":\"" RADIO_EXPECTED_FW "\",\"ok\":%s}}",
                     fw, RT_PROTO, fields,
                     s_rollback ? "true" : "false", s_nvs_wiped ? "true" : "false",
                     s_radio_fw, s_radio_ok ? "true" : "false");
    if (n < 0 || n >= (int)sizeof(buf)) {
        /* Same rule as the hello reply: truncated identity JSON parses as a different
           car (or as nothing), and shipping it under a 200 hides exactly that. Only
           reachable if a future field outgrows the buffer — then this is the symptom. */
        ESP_LOGE(TAG, "/status does not fit its buffer");
        return api_reply_error(req, "500 Internal Server Error", "", "status too long");
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, n);
}

esp_err_t status_api_start(void) {
    httpd_handle_t server = http_server_get_handle();
    if (server == NULL) { ESP_LOGE(TAG, "http server not started"); return ESP_FAIL; }
    read_radio_version();
    read_rollback_state();
    httpd_uri_t u = { .uri = "/status", .method = HTTP_GET, .handler = status_get };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &u), TAG, "reg /status");
    ESP_LOGI(TAG, "status endpoint registered");
    return ESP_OK;
}
