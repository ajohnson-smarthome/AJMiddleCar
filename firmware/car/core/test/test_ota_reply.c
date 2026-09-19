#include "../main/ota_reply.h"
#include "contract.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* The two IDF verdicts the map must tell apart, spelled the way esp_err.h and
 * esp_ota_ops.h spell them — ota_api.c static-asserts the copy in ota_reply.h against the
 * real headers, so a drift there fails the firmware build, not this test. */
#define IDF_FAIL            (-1)      /* ESP_FAIL: the flash driver gave up */
#define IDF_INVALID_SIZE    0x104     /* ESP_ERR_INVALID_SIZE: a partition write past the end */
#define IDF_NO_MEM          0x101     /* ESP_ERR_NO_MEM */
#define IDF_OTA_CONFLICT    0x1501    /* ESP_ERR_OTA_PARTITION_CONFLICT */

static void want(ota_step_t step, int err, const char *status, const char *code, const char *msg) {
    ota_reply_t r = ota_reply_for(step, err);
    if (strcmp(r.status, status) != 0 || strcmp(r.code, code) != 0 || strcmp(r.msg, msg) != 0) {
        printf("FAIL step %d err 0x%x: got (%s, %s, %s), want (%s, %s, %s)\n",
               (int)step, err, r.status, r.code, r.msg, status, code, msg);
        assert(0);
    }
}

int main(void) {
    /* The bug (AJM-37): the first block's rejected header is the body's fault, not the
       flash's — 400 not_firmware, the word the mock, conformance and protocol.md use. */
    want(OTA_STEP_WRITE, OTA_ERR_VALIDATE_FAILED,
         "400 Bad Request", ERR_NOT_FIRMWARE, "not an ESP image");
    /* Every other write failure is the flash refusing an acceptable block. */
    want(OTA_STEP_WRITE, IDF_FAIL,         "500 Internal Server Error", ERR_WRITE_FAILED, "ota write failed");
    want(OTA_STEP_WRITE, IDF_INVALID_SIZE, "500 Internal Server Error", ERR_WRITE_FAILED, "ota write failed");

    /* The slot could not be started: the board's fault, whatever the code. */
    want(OTA_STEP_BEGIN, IDF_OTA_CONFLICT, "500 Internal Server Error", ERR_WRITE_FAILED, "ota begin failed");
    want(OTA_STEP_BEGIN, IDF_NO_MEM,       "500 Internal Server Error", ERR_WRITE_FAILED, "ota begin failed");

    /* The image as a whole did not verify once written: still the body's fault. */
    want(OTA_STEP_END, OTA_ERR_VALIDATE_FAILED, "400 Bad Request", ERR_NOT_FIRMWARE, "image invalid");
    want(OTA_STEP_END, IDF_FAIL,                "400 Bad Request", ERR_NOT_FIRMWARE, "image invalid");

    /* Written and valid, but not made bootable: the board's fault. */
    want(OTA_STEP_SET_BOOT, IDF_FAIL,                "500 Internal Server Error", ERR_WRITE_FAILED, "set boot failed");
    want(OTA_STEP_SET_BOOT, OTA_ERR_VALIDATE_FAILED, "500 Internal Server Error", ERR_WRITE_FAILED, "set boot failed");

    /* The copied IDF value: 0x1500 (ESP_ERR_OTA_BASE) + 3. */
    assert(OTA_ERR_VALIDATE_FAILED == 0x1503);

    printf("test_ota_reply: all passed\n");
    return 0;
}
