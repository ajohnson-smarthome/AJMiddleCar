#include "api_util.h"
#include <stdio.h>
#include "dongle_contract.inc"

esp_err_t api_reply_error(httpd_req_t *req, const char *status, const char *code,
                          const char *field, const char *msg) {
    char buf[224];
    int n;
    /* `field` can be the client's own key echoed back (unknown_field), so it is cut at 48
       chars: an over-long one would push the envelope past the buffer and lose the reply.
       Not escaped — a quote inside a key is the client's problem, as before. */
    if (field && field[0]) {
        n = snprintf(buf, sizeof(buf),
                     "{\"" DONGLE_KEY_PROTO "\":%d,\"" DONGLE_KEY_ERROR "\":{\"" DONGLE_KEY_ERROR_CODE "\":\"%s\","
                     "\"" DONGLE_KEY_ERROR_MESSAGE "\":\"%s\",\"" DONGLE_KEY_ERROR_FIELD "\":\"%.48s\"}}",
                     DONGLE_PROTO, code, msg, field);
    } else {
        n = snprintf(buf, sizeof(buf),
                     "{\"" DONGLE_KEY_PROTO "\":%d,\"" DONGLE_KEY_ERROR "\":{\"" DONGLE_KEY_ERROR_CODE "\":\"%s\","
                     "\"" DONGLE_KEY_ERROR_MESSAGE "\":\"%s\"}}",
                     DONGLE_PROTO, code, msg);
    }
    if (n < 0 || n >= (int)sizeof(buf)) return ESP_FAIL;
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, n);
}

esp_err_t api_reply_ok(httpd_req_t *req) {
    return api_reply_json(req, "\"" DONGLE_KEY_OK "\":true");
}

esp_err_t api_reply_json(httpd_req_t *req, const char *members) {
    char buf[640];
    int n = snprintf(buf, sizeof(buf), "{\"" DONGLE_KEY_PROTO "\":%d,%s}", DONGLE_PROTO, members);
    if (n < 0 || n >= (int)sizeof(buf)) return ESP_FAIL;
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, n);
}

/* Read the whole body, however TCP chose to split it. A single httpd_req_recv call
   silently truncates a body arriving in two segments, which then fails with a message
   blaming the field names instead of the transport. */
int api_read_body(httpd_req_t *req, char *buf, size_t n) {
    if (req->content_len <= 0 || (size_t)req->content_len >= n) return -1;
    size_t got = 0;
    int timeouts = 0;
    while (got < (size_t)req->content_len) {
        int r = httpd_req_recv(req, buf + got, (size_t)req->content_len - got);
        if (r > 0) { got += (size_t)r; timeouts = 0; continue; }
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts <= 3) continue;
        return -1;
    }
    buf[got] = '\0';
    return (int)got;
}
