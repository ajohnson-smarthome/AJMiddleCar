#include "api_util.h"
#include <stdio.h>

esp_err_t api_reply_error(httpd_req_t *req, const char *status, const char *field,
                          const char *msg) {
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"error\":\"%s\",\"field\":\"%s\"}", msg, field);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

esp_err_t api_reply_ok(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/* Read the whole body, however TCP chose to split it. The five handlers this file
   replaces each made a single httpd_req_recv call, so a body arriving in two segments
   was silently truncated and then rejected with a message blaming the field names. */
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
