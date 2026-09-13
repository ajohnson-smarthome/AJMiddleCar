#ifndef API_UTIL_H
#define API_UTIL_H

#include <stddef.h>
#include "esp_err.h"
#include "esp_http_server.h"

/* The REST surface's shared plumbing. One envelope for every endpoint —
 * {"proto":2,"error":{"code":"...","message":"...","field":"..."}}, field omitted when
 * the fault is with the body as a whole — because the car and the mock answering
 * different shapes let a client work in the simulator and fail to parse the hardware.
 * And one body reader, because "read the whole body, however TCP split it" was fixed in
 * cfg_api and the single-recv copy in calib_api kept truncating segmented bodies into a
 * 400 that blamed the field names. */

/* {"proto":2,"error":{"code":"<code>","message":"<msg>","field":"<field>"}} — code is
 * one of the ERR_* words; field is omitted when "" (the whole body is at fault). */
esp_err_t api_reply_error(httpd_req_t *req, const char *status, const char *code,
                          const char *field, const char *msg);

/* {"proto":2,"ok":true} with the JSON content type — the documented success body. */
esp_err_t api_reply_ok(httpd_req_t *req);

/* {"proto":2,<members>} — the caller supplies the members without the outer braces and
 * without a leading comma; the envelope's proto is prepended here so no endpoint can
 * forget it. */
esp_err_t api_reply_json(httpd_req_t *req, const char *members);

/* Read the whole body into buf (NUL-terminated). Returns the length, or -1 when the
 * body is absent, too long for buf, or the socket gave up. */
int api_read_body(httpd_req_t *req, char *buf, size_t n);

#endif /* API_UTIL_H */
