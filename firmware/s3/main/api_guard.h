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
 * outright unless it returns ESP_OK — refusing the connection rather than the request. It
 * returns ESP_OK only when getsockname reports DONGLE_HOST; every other case, including a
 * getsockname failure, returns ESP_FAIL and fails closed.
 *
 * What that establishes, precisely: the connection was ADDRESSED to DONGLE_HOST. It does not
 * establish which wire it arrived on, and an earlier version of this comment claimed it did.
 * For an accepted pcb lwIP copies local_ip straight out of the SYN's destination field, and
 * lwIP is a weak-host stack (ip4_input walks NETIF_FOREACH and accepts on any netif whose
 * address matches), so a SYN from the car's network addressed to 192.168.7.1:8080 would yield
 * getsockname == 192.168.7.1 and satisfy this check. The relays do not share the weakness:
 * they pin their listeners to the USB interface with SO_BINDTODEVICE (usb_net_bind_socket),
 * which is a genuine arrival-interface filter. esp_http_server creates its listener inside
 * httpd_start and exposes neither a bind address nor the listening fd, so the same pin cannot
 * be applied at the one place it would settle the question. IDF 6.0.2 offers nothing that
 * answers it from an accepted socket either: lwIP implements SO_BINDTODEVICE for setsockopt
 * only — lwip_getsockopt_impl has no case for it and returns ENOPROTOOPT — and an accepted
 * pcb inherits netif_idx from the listener (tcp_in.c:711), which esp_http_server leaves
 * unbound, so there would be nothing to read back even if it could be read. A netif lookup by
 * address answers "which interface OWNS 192.168.7.1", which under weak-host is a different
 * question from "which interface did this arrive on".
 *
 * Two things do stand behind the check, and both are named here because neither is this file:
 *
 * 1. The USB netif has no gateway (usb_net.c leaves ip.gw at 0.0.0.0). A SYN-ACK to a
 *    car-side peer is routed by SOURCE address — IDF's ip4_route_src_hook — onto the USB
 *    netif, where etharp_output has no gateway through which to send an off-link frame and
 *    fails, so the handshake never completes and open_fn never runs for that peer. A real
 *    control, but an accidental one: it rests on a usability decision, and usb_net.c's ip.gw
 *    comment records that this guard now depends on it.
 * 2. Every connection this function admits is pinned to the USB interface on the way out (see
 *    the .c). From then on lwIP discards any segment for that connection which arrived on
 *    another netif, so nothing a handler reads afterwards can have come from the car's
 *    network. It does not cover bytes lwIP had already queued between the handshake
 *    completing and open_fn running — a request pipelined into the first segment burst. That
 *    narrows the window to one segment; it does not close it.
 *
 * That local address does not arrive as a plain IPv4 sockaddr: esp_http_server's listener is
 * AF_INET6 whenever CONFIG_LWIP_IPV6=y (IDF 6.0.2's own default here), so an IPv4 connection's
 * local and peer addresses both come back IPv4-mapped (::ffff:a.b.c.d, family AF_INET6). The
 * .c file's sockaddr_to_ipv4 handles both families explicitly rather than assuming either —
 * see its comment for why that is done instead of turning CONFIG_LWIP_IPV6 off. */
esp_err_t api_guard_open(httpd_handle_t hd, int sockfd);

#endif /* API_GUARD_H */
