#include "ota_reply.h"
#include "dongle_contract.inc"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* The IDF verdicts the map must tell apart, spelled the way esp_err.h and esp_ota_ops.h
 * spell them — ota_api.c static-asserts the copies in ota_reply.h against the real headers,
 * so a drift there fails the firmware build, not this test. */
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
    /* The bug (AJM-137): every refusal past the size checks answered with an envelope and
       then returned ESP_FAIL, which closes the session with the body unread — RST, and the
       client saw a transport error instead of the code. The map is what ota_api.c now
       returns the result of, one call per step; each step's verdict is pinned here. */

    /* The slot could not be started: the board's fault, whatever the code — except the one
       code that means "the running image has not finished verifying its own boot", which is
       a refusal to start a second update over an unconfirmed one: busy, 409. */
    want(OTA_STEP_BEGIN, IDF_OTA_CONFLICT, "500 Internal Server Error", DONGLE_ERR_WRITE_FAILED, "ota begin failed");
    want(OTA_STEP_BEGIN, IDF_NO_MEM,       "500 Internal Server Error", DONGLE_ERR_WRITE_FAILED, "ota begin failed");
    want(OTA_STEP_BEGIN, IDF_FAIL,         "500 Internal Server Error", DONGLE_ERR_WRITE_FAILED, "ota begin failed");
    want(OTA_STEP_BEGIN, OTA_ERR_ROLLBACK_INVALID_STATE,
         "409 Conflict", DONGLE_ERR_BUSY, "image still pending verify");

    /* The first block's rejected header is the body's fault, not the flash's — 400
       not_firmware, the word the app's firmware screen names. */
    want(OTA_STEP_WRITE, OTA_ERR_VALIDATE_FAILED,
         "400 Bad Request", DONGLE_ERR_NOT_FIRMWARE, "not an ESP image");
    /* Every other write failure is the flash refusing an acceptable block. */
    want(OTA_STEP_WRITE, IDF_FAIL,         "500 Internal Server Error", DONGLE_ERR_WRITE_FAILED, "ota write failed");
    want(OTA_STEP_WRITE, IDF_INVALID_SIZE, "500 Internal Server Error", DONGLE_ERR_WRITE_FAILED, "ota write failed");
    /* The pending-verify code is meaningful only at begin; at write it is just a refusal. */
    want(OTA_STEP_WRITE, OTA_ERR_ROLLBACK_INVALID_STATE,
         "500 Internal Server Error", DONGLE_ERR_WRITE_FAILED, "ota write failed");

    /* The image as a whole did not verify once written: still the body's fault. */
    want(OTA_STEP_END, OTA_ERR_VALIDATE_FAILED, "400 Bad Request", DONGLE_ERR_NOT_FIRMWARE, "image invalid");
    want(OTA_STEP_END, IDF_FAIL,                "400 Bad Request", DONGLE_ERR_NOT_FIRMWARE, "image invalid");

    /* Written and valid, but not made bootable: the board's fault. */
    want(OTA_STEP_SET_BOOT, IDF_FAIL,                "500 Internal Server Error", DONGLE_ERR_WRITE_FAILED, "set boot failed");
    want(OTA_STEP_SET_BOOT, OTA_ERR_VALIDATE_FAILED, "500 Internal Server Error", DONGLE_ERR_WRITE_FAILED, "set boot failed");

    /* The copied IDF values: ESP_ERR_OTA_BASE (0x1500) + 3 and + 6. */
    assert(OTA_ERR_VALIDATE_FAILED == 0x1503);
    assert(OTA_ERR_ROLLBACK_INVALID_STATE == 0x1506);

    printf("test_ota_reply: all passed\n");
    return 0;
}
