#ifndef OTA_REPLY_H
#define OTA_REPLY_H

#include "contract.h"   /* ERR_NOT_FIRMWARE, ERR_WRITE_FAILED */

/* Which error envelope a refused esp_ota_* call gets. Pure: the decision "is this the
 * body's fault (400, take another image) or the board's (500, the same image may be
 * retried)" is made here from the step and the IDF code, and ota_api.c only sends what
 * comes back. Written inline it once answered a rejected first block as a flash failure
 * (AJM-37) — the header check lives inside esp_ota_write, so the step alone cannot tell
 * "not an image" from "flash refused the write"; the code can. */

/* ESP_ERR_OTA_VALIDATE_FAILED — ESP_ERR_OTA_BASE (0x1500) + 3 — copied so this header has
 * no IDF dependency and the map is host-tested; ota_api.c static-asserts it against
 * esp_ota_ops.h, so the copy cannot drift silently. esp_ota_write returns it for a first
 * block whose byte 0 is not the app image magic, esp_ota_end for an image that fails
 * verification as a whole. */
#define OTA_ERR_VALIDATE_FAILED 0x1503

typedef enum {
    OTA_STEP_BEGIN,      /* esp_ota_begin: the slot could not be started */
    OTA_STEP_WRITE,      /* esp_ota_write: a block was refused */
    OTA_STEP_END,        /* esp_ota_end: the written image did not verify */
    OTA_STEP_SET_BOOT,   /* esp_ota_set_boot_partition: written and valid, not bootable */
} ota_step_t;

typedef struct {
    const char *status;   /* the HTTP status line */
    const char *code;     /* one of the contract's ERR_* words */
    const char *msg;      /* the envelope's message — for the log, not the screen */
} ota_reply_t;

#define OTA_REPLY_BODY  "400 Bad Request"
#define OTA_REPLY_BOARD "500 Internal Server Error"

/* `err` is the failed call's return, never ESP_OK. Only the write step looks at it: begin
 * and set-boot never blame the body, and end never blames the board — whatever verify
 * refused, the client's fix is another image. */
static inline ota_reply_t ota_reply_for(ota_step_t step, int err) {
    switch (step) {
    case OTA_STEP_BEGIN:    return (ota_reply_t){ OTA_REPLY_BOARD, ERR_WRITE_FAILED, "ota begin failed" };
    case OTA_STEP_WRITE:
        if (err == OTA_ERR_VALIDATE_FAILED) {
            return (ota_reply_t){ OTA_REPLY_BODY,  ERR_NOT_FIRMWARE, "not an ESP image" };
        }
        return (ota_reply_t){ OTA_REPLY_BOARD, ERR_WRITE_FAILED, "ota write failed" };
    case OTA_STEP_END:      return (ota_reply_t){ OTA_REPLY_BODY,  ERR_NOT_FIRMWARE, "image invalid" };
    case OTA_STEP_SET_BOOT:
    default:                return (ota_reply_t){ OTA_REPLY_BOARD, ERR_WRITE_FAILED, "set boot failed" };
    }
}

#endif /* OTA_REPLY_H */
