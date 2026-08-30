#ifndef API_GUARD_H
#define API_GUARD_H

#include "esp_err.h"
#include "esp_http_server.h"

/* The one thing standing between a station netif and a Wi-Fi password.
 *
 * httpd_config_t in ESP-IDF 6.0.2 has no bind-address field, so status_api's server always
 * listens on INADDR_ANY — every interface the dongle has, USB and (since the station came up)
 * the car's Wi-Fi alike. GET /status is harmless to expose; POST /net carries a password and
 * POST /ota writes firmware with no authentication, so both must answer only on the USB wire.
 *
 * Assign this to httpd_config_t.open_fn before httpd_start. It runs on every accepted
 * connection, before a single request byte is parsed, and esp_http_server closes the socket
 * outright unless it returns ESP_OK — refusing the connection rather than the request. Checks
 * getsockname, not getpeername: the local address says which wire the connection came in on,
 * which is the only question that matters, while the peer's claimed address is a subnet guess
 * and not a security boundary. Returns ESP_OK only when the local address is DONGLE_HOST;
 * every other case, including a getsockname failure, returns ESP_FAIL and fails closed.
 *
 * That local address does not arrive as a plain IPv4 sockaddr: esp_http_server's listener is
 * AF_INET6 whenever CONFIG_LWIP_IPV6=y (IDF 6.0.2's own default here), so an IPv4 connection's
 * local and peer addresses both come back IPv4-mapped (::ffff:a.b.c.d, family AF_INET6). The
 * .c file's sockaddr_to_ipv4 handles both families explicitly rather than assuming either —
 * see its comment for why that is done instead of turning CONFIG_LWIP_IPV6 off. */
esp_err_t api_guard_open(httpd_handle_t hd, int sockfd);

#endif /* API_GUARD_H */
