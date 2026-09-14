#include "status_api.h"
#include <stdio.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_app_desc.h"
#include "http_server.h"
#include "telemetry.h"
#include "device_json.h"
#include "board.h"
#include "contract.h"
#include <string.h>
#include "esp_ota_ops.h"
#include "api_util.h"
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
// the handler — then only read, so the cross-task safety is ordering, not a lock. Empty
// means the radio did not answer; radio_state_word() below is what turns that into the
// wire's "unavailable".
static char s_radio_fw[24] = "";
static bool s_radio_ok     = false;

/* Did the bootloader revert the previous OTA? The other slot is left ESP_OTA_IMG_ABORTED
 * exactly when an update failed its first boot — the one signal a client has that the
 * image it flashed did not survive. Read once, lazily, and cached: the answer cannot
 * change without a reboot, and rt_link_start runs before status_api_start, so the hello
 * reply's first caller must be able to trigger the read itself. */
static bool s_rollback      = false;
static bool s_rollback_read = false;
static bool s_nvs_wiped     = false;

void status_api_note_nvs_wiped(void) { s_nvs_wiped = true; }

bool status_api_rolled_back(void) {
    if (!s_rollback_read) {
        const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
        esp_ota_img_states_t st;
        s_rollback = other != NULL &&
                     esp_ota_get_state_partition(other, &st) == ESP_OK &&
                     st == ESP_OTA_IMG_ABORTED;
        s_rollback_read = true;
        if (s_rollback) ESP_LOGW(TAG, "the previous OTA was rolled back by the bootloader");
    }
    return s_rollback;
}

static void read_radio_version(void) {
    const char *v = radio_flash_version();
    if (strcmp(v, "unavailable") == 0) {
        s_radio_fw[0] = '\0';
        ESP_LOGW(TAG, "radio firmware unavailable — the RPC to the C6 did not answer");
        return;
    }
    snprintf(s_radio_fw, sizeof(s_radio_fw), "%s", v);
    s_radio_ok = (strcmp(s_radio_fw, RADIO_EXPECTED_FW) == 0);
    if (s_radio_ok) {
        ESP_LOGI(TAG, "radio firmware %s", s_radio_fw);
    } else {
        /* No longer an instruction to fetch a cable: main.c's boot gate offers the embedded
         * image first, and only a spent attempt budget or a build with no image reaches here
         * still mismatched. */
        ESP_LOGW(TAG, "radio firmware %s, expected %s — the car could not correct it; "
                      "the manual route is firmware/car/modem/README.md",
                 s_radio_fw, RADIO_EXPECTED_FW);
    }
}

static const char *radio_state_word(void) {
    if (s_radio_fw[0] == '\0') return RADIO_STATE_UNAVAILABLE;
    return s_radio_ok ? RADIO_STATE_OK : RADIO_STATE_MISMATCH;
}

static esp_err_t status_get(httpd_req_t *req) {
    telemetry_t t;
    telemetry_gather(&t, TELEM_STATUS);
    char device[160];
    if (device_group_json(device, sizeof(device), esp_app_get_description()->version,
                          status_api_rolled_back()) < 0) {
        ESP_LOGE(TAG, "/status could not render the device group");
        return api_reply_error(req, "500 Internal Server Error", ERR_INTERNAL, "", "identity too long");
    }
    char groups[384];
    if (telemetry_groups(groups, sizeof(groups), &t) < 0) {
        ESP_LOGE(TAG, "/status could not render its telemetry groups");
        return api_reply_error(req, "500 Internal Server Error", ERR_INTERNAL, "", "telemetry unavailable");
    }
    /* telemetry_groups prints link, motors, system, video. The schema's status order is
       device, link, motors, radio, storage, system, video — so the system member is
       split off the tail and radio/storage go in before it, and everything after system
       (video) rides with it. Splitting on the last group's opening key keeps the three
       words spelled by the one printer telemetry uses. */
    char *sys = strstr(groups, "\"" KEY_GROUP_SYSTEM "\":{");
    if (!sys || sys == groups || sys[-1] != ',') {
        ESP_LOGE(TAG, "/status could not find the system group");
        return api_reply_error(req, "500 Internal Server Error", ERR_INTERNAL, "", "status malformed");
    }
    sys[-1] = '\0';                       /* groups is now link,motors; sys is system */
    char radio_fw[32];
    if (s_radio_fw[0]) snprintf(radio_fw, sizeof(radio_fw), "\"%s\"", s_radio_fw);
    else               snprintf(radio_fw, sizeof(radio_fw), "null");
    char members[720];
    int n = snprintf(members, sizeof(members),
                     "%s,%s,"
                     "\"" KEY_GROUP_RADIO "\":{\"" KEY_RADIO_FW "\":%s,"
                         "\"" KEY_RADIO_EXPECTED "\":\"" RADIO_EXPECTED_FW "\","
                         "\"" KEY_RADIO_STATE "\":\"%s\"},"
                     "\"" KEY_GROUP_STORAGE "\":{\"" KEY_STORAGE_RESET_AT_BOOT "\":%s},"
                     "%s",
                     device, groups, radio_fw, radio_state_word(),
                     s_nvs_wiped ? "true" : "false", sys);
    if (n < 0 || n >= (int)sizeof(members)) {
        ESP_LOGE(TAG, "/status does not fit its buffer");
        return api_reply_error(req, "500 Internal Server Error", ERR_INTERNAL, "", "status too long");
    }
    return api_reply_json(req, members);
}

esp_err_t status_api_start(void) {
    httpd_handle_t server = http_server_get_handle();
    if (server == NULL) { ESP_LOGE(TAG, "http server not started"); return ESP_FAIL; }
    read_radio_version();
    status_api_rolled_back();
    httpd_uri_t u = { .uri = PATH_STATUS, .method = HTTP_GET, .handler = status_get };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &u), TAG, "reg " PATH_STATUS);
    ESP_LOGI(TAG, "status endpoint registered");
    return ESP_OK;
}
