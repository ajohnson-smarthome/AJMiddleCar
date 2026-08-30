#ifndef API_UTIL_H
#define API_UTIL_H

#include <stddef.h>
#include "esp_err.h"
#include "esp_http_server.h"

/* The REST surface's shared plumbing: one error shape for every endpoint, and one body
 * reader that copes with a body TCP split across segments.
 *
 * A deliberate twin of firmware/p4/main/api_util.{c,h} rather than a shared file. The two
 * firmwares do not reference each other — that independence is what lets the dongle stay
 * ignorant of the car — and the price of it is this much duplication, paid knowingly. */

/* {"error":"<msg>","field":"<field>"} with `status` as the HTTP status line.
 * `field` is "" when the body as a whole is at fault. */
esp_err_t api_reply_error(httpd_req_t *req, const char *status, const char *field,
                          const char *msg);

/* {"ok":true} with the JSON content type — the documented success body. */
esp_err_t api_reply_ok(httpd_req_t *req);

/* Read the whole body into buf (NUL-terminated). Returns the length, or -1 when the body
 * is absent, too long for buf, or the socket gave up. */
int api_read_body(httpd_req_t *req, char *buf, size_t n);

#endif /* API_UTIL_H */
