#include "api_guard.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "dongle_contract.inc"

static const char *TAG = "api_guard";

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

/* DONGLE_HOST is a compile-time string constant; parsed once and cached rather than on every
 * accepted connection. A parse failure here means the constant itself is malformed, not
 * anything about the connection being checked — fail closed exactly like every other branch
 * below, since a guard that cannot know the address it is defending is not a guard.
 *
 * Logged once, here, at ESP_LOGE — a build-time bug, not a per-connection event. That alone
 * would go silent after the very first accepted connection (this function only runs once,
 * cached by `have`), which is exactly wrong for a state that means the whole API is
 * permanently unreachable: api_guard_open folds a `false` return from this function into its
 * own rate-limited rejection log below, so a broken constant stays visible on the console on
 * a 1 Hz cadence instead of vanishing after one line. */
static bool want_addr(esp_ip4_addr_t *out)
{
    static bool have;
    static esp_ip4_addr_t want;
    static bool ok;

    if (!have) {
        ok = esp_netif_str_to_ip4(DONGLE_HOST, &want) == ESP_OK;
        if (!ok) ESP_LOGE(TAG, "DONGLE_HOST does not parse as an address");
        have = true;
    }
    *out = want;
    return ok;
}

/* esp_http_server's listener is AF_INET6 whenever CONFIG_LWIP_IPV6=y — IDF 6.0.2's own
 * default (firmware/s3/sdkconfig: CONFIG_LWIP_IPV6=y; nothing this project set), and
 * esp_http_server's httpd_server_init picks `socket(PF_INET6, ...)` under exactly that
 * `#if CONFIG_LWIP_IPV6` (esp_http_server/src/httpd_main.c). Every accepted connection then
 * inherits an IPv6 netconn (lwIP's api_msg.c: the new netconn is allocated with the
 * listener's own type), and lwip_getaddrname — which backs both getsockname and getpeername
 * (lwip/api/sockets.c) — reports an IPv4 peer as an IPv4-mapped IPv6 address
 * (::ffff:a.b.c.d, family AF_INET6, not a plain sockaddr_in with AF_INET). A guard that only
 * ever compared sin_family == AF_INET would see family 10 (AF_INET6) on every single
 * connection and never match — refusing the USB wire along with everything else, silently,
 * since the failure is a clean ESP_FAIL with no crash or overflow to notice.
 *
 * This is deliberately NOT fixed by setting CONFIG_LWIP_IPV6=n. That would work today, but it
 * would make a security control's correctness depend on a config flag staying exactly where
 * it is — a later, unrelated change to sdkconfig.defaults could silently re-break the guard
 * with nothing to catch it (the build still succeeds; only the accept/reject decision
 * changes, in a way no host test can see). Handling both address families explicitly, here,
 * stays correct whichever way that flag moves: this function returns false for the plain
 * AF_INET case only if CONFIG_LWIP_IPV6 were ever off and something else were badly wrong,
 * and the AF_INET6/v4-mapped case only exists at all because IPv6 is on.
 *
 * The relays (relay_udp.c, relay_tcp.c) do not have this problem: they each call
 * socket(AF_INET, ...) themselves and bind DONGLE_HOST directly, so they only ever see plain
 * IPv4 sockaddrs regardless of CONFIG_LWIP_IPV6. esp_http_server is the one place in this
 * firmware that opens its own listener rather than being told which family to use — which is
 * exactly why it is the trap. */
static bool sockaddr_to_ipv4(const struct sockaddr_storage *ss, esp_ip4_addr_t *out)
{
    if (ss->ss_family == AF_INET) {
        out->addr = ((const struct sockaddr_in *)ss)->sin_addr.s_addr;
        return true;
    }
#if LWIP_IPV6
    if (ss->ss_family == AF_INET6) {
        const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)ss;
        if (IN6_IS_ADDR_V4MAPPED(&sin6->sin6_addr)) {
            /* The low 32 bits of a v4-mapped address ARE the IPv4 address, already in the
             * same network byte order esp_netif_str_to_ip4 and sin_addr.s_addr use — lwIP's
             * own ip4_2_ipv4_mapped_ipv6 (lwip/ip_addr.h) does this exact assignment with no
             * byte swap when it builds one. A plain memcpy, not a reinterpretation. */
            memcpy(&out->addr, &sin6->sin6_addr.s6_addr[12], sizeof(out->addr));
            return true;
        }
    }
#endif /* LWIP_IPV6 */
    /* A genuine (non-mapped) IPv6 address, or any other family: nothing to compare against
     * DONGLE_HOST. Returning false here always ends in api_guard_open's ESP_FAIL branch —
     * never in ESP_OK — which is correct: this guard has no IPv6 identity for DONGLE_HOST to
     * admit a connection against. */
    return false;
}

esp_err_t api_guard_open(httpd_handle_t hd, int sockfd)
{
    (void)hd;

    esp_ip4_addr_t want;
    bool have_want = want_addr(&want);

    struct sockaddr_storage local;
    socklen_t local_len = sizeof(local);
    bool got_local = getsockname(sockfd, (struct sockaddr *)&local, &local_len) == 0;

    esp_ip4_addr_t local_ip;
    if (have_want && got_local && sockaddr_to_ipv4(&local, &local_ip) &&
        local_ip.addr == want.addr) {
        return ESP_OK;
    }

    /* Rejected. This runs on an accept path an attacker controls, and the dongle's only
     * console is a single shared UART, so THIS file's own log line is rate-limited (same
     * last_log idiom as relay_udp.c and relay_tcp.c) — but that does not make rejection quiet
     * on its own: esp_http_server logs unthrottled on top of it regardless. A non-ESP_OK
     * return from open_fn makes httpd_sess_new log ESP_LOGE("session creation failed")
     * (esp_http_server/src/httpd_sess.c) and its caller, httpd_accept_conn, log
     * ESP_LOGW("error accepting new connection") right after
     * (esp_http_server/src/httpd_main.c) — two more lines per rejection, both unthrottled,
     * both printed at CONFIG_LOG_DEFAULT_LEVEL=3. This line is one of three, not the only
     * one. Do not reach for esp_log_level_set on esp_http_server's tag to quiet the other two
     * — that costs that tag's other diagnostics on a device whose UART is its only window, to
     * defend against a peer that must already be on the car's WPA2 network to reach this at
     * all.
     *
     * Seeded one interval in the past, not zero: a guard's very first rejections — in the
     * first second after boot — are exactly the interesting ones, and last_log starting at 0
     * would silently drop whichever of them land before now_ms() first exceeds 1000. */
    static uint32_t last_log = (uint32_t)-1001;
    uint32_t t = now_ms();
    if ((uint32_t)(t - last_log) > 1000) {
        last_log = t;
        if (!have_want) {
            ESP_LOGW(TAG, "refusing every connection: DONGLE_HOST does not parse");
        } else if (!got_local) {
            ESP_LOGW(TAG, "getsockname failed on an accepted socket: errno %d, rejecting", errno);
        } else {
            struct sockaddr_storage peer;
            socklen_t peer_len = sizeof(peer);
            esp_ip4_addr_t peer_ip;
            uint16_t peer_port = 0;
            bool have_peer = getpeername(sockfd, (struct sockaddr *)&peer, &peer_len) == 0 &&
                              sockaddr_to_ipv4(&peer, &peer_ip);
            if (have_peer) {
                if (peer.ss_family == AF_INET) {
                    peer_port = ntohs(((struct sockaddr_in *)&peer)->sin_port);
                }
#if LWIP_IPV6
                else if (peer.ss_family == AF_INET6) {
                    peer_port = ntohs(((struct sockaddr_in6 *)&peer)->sin6_port);
                }
#endif /* LWIP_IPV6 */
            }
            if (have_peer) {
                ESP_LOGW(TAG, "rejected " IPSTR ":%u - connection did not land on DONGLE_HOST",
                         IP2STR(&peer_ip), (unsigned)peer_port);
            } else {
                ESP_LOGW(TAG, "rejected a connection not on DONGLE_HOST (peer unknown)");
            }
        }
    }

    return ESP_FAIL;
}
