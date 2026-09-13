#ifndef API_UTIL_H
#define API_UTIL_H

#include <stddef.h>
#include "esp_err.h"
#include "esp_http_server.h"

/* The REST surface's shared plumbing. One envelope for every endpoint —
 * {"proto":1,"error":{"code":"...","message":"...","field":"..."}}, field omitted when
 * the fault is with the body as a whole — because the dongle and a test double answering
 * different shapes let a client work against one and fail to parse the other. And one
 * body reader, because "read the whole body, however TCP split it" belongs in one place
 * rather than being re-solved by every handler that takes a POST.
 *
 * A deliberate twin of firmware/car/core/main/api_util.{c,h} rather than a shared file. The two
 * firmwares do not reference each other — that independence is what lets the dongle stay
 * ignorant of the car — and the price of it is this much duplication, paid knowingly. */

/* {"proto":1,"error":{"code":"<code>","message":"<msg>","field":"<field>"}} — code is
 * one of the DONGLE_ERR_* words; field is omitted when "" (the whole body is at fault). */
esp_err_t api_reply_error(httpd_req_t *req, const char *status, const char *code,
                          const char *field, const char *msg);

/* {"proto":1,"ok":true} with the JSON content type — the documented success body. */
esp_err_t api_reply_ok(httpd_req_t *req);

/* {"proto":1,<members>} — the caller supplies the members without the outer braces and
 * without a leading comma; the envelope's proto is prepended here so no endpoint can
 * forget it. */
esp_err_t api_reply_json(httpd_req_t *req, const char *members);

/* Read the whole body into buf (NUL-terminated). Returns the length, or -1 when the body
 * is absent, too long for buf, or the socket gave up. */
int api_read_body(httpd_req_t *req, char *buf, size_t n);

#endif /* API_UTIL_H */
