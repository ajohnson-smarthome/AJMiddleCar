#include "api_util.h"

#include <stdio.h>

esp_err_t api_reply_error(httpd_req_t *req, const char *status, const char *field,
                          const char *msg)
{
    char body[192];
    int n = snprintf(body, sizeof(body), "{\"error\":\"%s\",\"field\":\"%s\"}", msg, field);
    if (n < 0 || (size_t)n >= sizeof(body)) {
        return ESP_FAIL;
    }
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, n);
}

esp_err_t api_reply_ok(httpd_req_t *req)
{
    static const char ok[] = "{\"ok\":true}";
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, ok, sizeof(ok) - 1);
}

int api_read_body(httpd_req_t *req, char *buf, size_t n)
{
    if (req->content_len <= 0 || (size_t)req->content_len >= n) {
        return -1;
    }
    /* One recv can return a prefix: a body split across TCP segments must be gathered,
     * not truncated into a 400 that blames the field names. A bounded run of
     * HTTPD_SOCK_ERR_TIMEOUT is retried rather than treated as failure for the same
     * reason — a slow client mid-body is not the same fault as a missing one, and the
     * car's twin of this function paid for that distinction in debugging. */
    size_t got = 0;
    int timeouts = 0;
    while (got < (size_t)req->content_len) {
        int r = httpd_req_recv(req, buf + got, (size_t)req->content_len - got);
        if (r > 0) {
            got += (size_t)r;
            timeouts = 0;
            continue;
        }
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts <= 3) {
            continue;
        }
        return -1;
    }
    buf[got] = '\0';
    return (int)got;
}
