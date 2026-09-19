#ifndef OTA_REPLY_H
#define OTA_REPLY_H

#include "dongle_contract.inc"   /* DONGLE_ERR_NOT_FIRMWARE, DONGLE_ERR_WRITE_FAILED, DONGLE_ERR_BUSY */

/* Which error envelope a refused esp_ota_* call gets. Pure: the decision "is this the
 * body's fault (400, take another image), the board's (500, the same image may be retried),
 * or a refusal to start over an image that has not confirmed itself (409, try later)" is
 * made here from the step and the IDF code, and ota_api.c only sends what comes back —
 * and returns the send's result, never ESP_FAIL. Written inline, the handler answered
 * every refusal past the size checks with an envelope and then ESP_FAIL, which makes httpd
 * close the session with the body unread: TCP answers that with RST, and the envelope
 * never reached the client (AJM-137).
 *
 * A deliberate twin of firmware/car/core/main/ota_reply.h, not a shared file — the two
 * firmwares do not reference each other. The one difference is the begin step's `busy`:
 * the car's app_main cancels rollback the same way, so the race exists there too, but only
 * the dongle answers it by name. */

/* Copied so this header has no IDF dependency and the map is host-tested; ota_api.c
 * static-asserts both against esp_ota_ops.h, so a copy cannot drift silently.
 * ESP_ERR_OTA_VALIDATE_FAILED — ESP_ERR_OTA_BASE (0x1500) + 3: esp_ota_write returns it for
 * a first block whose byte 0 is not the app image magic, esp_ota_end for an image that
 * fails verification as a whole. ESP_ERR_OTA_ROLLBACK_INVALID_STATE — + 6: esp_ota_begin
 * returns it while the running image is still PENDING_VERIFY. */
#define OTA_ERR_VALIDATE_FAILED         0x1503
#define OTA_ERR_ROLLBACK_INVALID_STATE  0x1506

typedef enum {
    OTA_STEP_BEGIN,      /* esp_ota_begin: the slot could not be started */
    OTA_STEP_WRITE,      /* esp_ota_write: a block was refused */
    OTA_STEP_END,        /* esp_ota_end: the written image did not verify */
    OTA_STEP_SET_BOOT,   /* esp_ota_set_boot_partition: written and valid, not bootable */
} ota_step_t;

typedef struct {
    const char *status;   /* the HTTP status line */
    const char *code;     /* one of the contract's DONGLE_ERR_* words */
    const char *msg;      /* the envelope's message — for the log, not the screen */
} ota_reply_t;

#define OTA_REPLY_BODY  "400 Bad Request"
#define OTA_REPLY_BOARD "500 Internal Server Error"
#define OTA_REPLY_LATER "409 Conflict"

/* `err` is the failed call's return, never ESP_OK. Begin and write look at it: begin tells
 * "the running image has not verified its own boot" from every other refusal to start, write
 * tells "not an image" from "flash refused the block". Set-boot never blames the body, and
 * end never blames the board — whatever verify refused, the client's fix is another image. */
static inline ota_reply_t ota_reply_for(ota_step_t step, int err) {
    switch (step) {
    case OTA_STEP_BEGIN:
        if (err == OTA_ERR_ROLLBACK_INVALID_STATE) {
            return (ota_reply_t){ OTA_REPLY_LATER, DONGLE_ERR_BUSY, "image still pending verify" };
        }
        return (ota_reply_t){ OTA_REPLY_BOARD, DONGLE_ERR_WRITE_FAILED, "ota begin failed" };
    case OTA_STEP_WRITE:
        if (err == OTA_ERR_VALIDATE_FAILED) {
            return (ota_reply_t){ OTA_REPLY_BODY,  DONGLE_ERR_NOT_FIRMWARE, "not an ESP image" };
        }
        return (ota_reply_t){ OTA_REPLY_BOARD, DONGLE_ERR_WRITE_FAILED, "ota write failed" };
    case OTA_STEP_END:      return (ota_reply_t){ OTA_REPLY_BODY,  DONGLE_ERR_NOT_FIRMWARE, "image invalid" };
    case OTA_STEP_SET_BOOT:
    default:                return (ota_reply_t){ OTA_REPLY_BOARD, DONGLE_ERR_WRITE_FAILED, "set boot failed" };
    }
}

#endif /* OTA_REPLY_H */
