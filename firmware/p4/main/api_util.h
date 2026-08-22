#ifndef API_UTIL_H
#define API_UTIL_H

#include <stddef.h>
#include "esp_err.h"
#include "esp_http_server.h"

/* The REST surface's shared plumbing. One error shape for every endpoint —
 * {"error":"...","field":"..."}, field "" when the fault is with the body as a whole —
 * because the car and the mock answering different shapes let a client work in the
 * simulator and fail to parse the hardware. And one body reader, because "read the
 * whole body, however TCP split it" was fixed in cfg_api and the single-recv copy in
 * calib_api kept truncating segmented bodies into a 400 that blamed the field names. */

esp_err_t api_reply_error(httpd_req_t *req, const char *status, const char *field,
                          const char *msg);

/* {"ok":true} with the JSON content type — the documented success body. */
esp_err_t api_reply_ok(httpd_req_t *req);

/* Read the whole body into buf (NUL-terminated). Returns the length, or -1 when the
 * body is absent, too long for buf, or the socket gave up. */
int api_read_body(httpd_req_t *req, char *buf, size_t n);

#endif /* API_UTIL_H */
