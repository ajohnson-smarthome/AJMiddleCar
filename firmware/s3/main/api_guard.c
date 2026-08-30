#include "api_guard.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
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
 * below, since a guard that cannot know the address it is defending is not a guard. */
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

esp_err_t api_guard_open(httpd_handle_t hd, int sockfd)
{
    (void)hd;

    esp_ip4_addr_t want;
    if (!want_addr(&want)) {
        return ESP_FAIL;
    }

    struct sockaddr_in local;
    socklen_t local_len = sizeof(local);
    if (getsockname(sockfd, (struct sockaddr *)&local, &local_len) < 0) {
        /* Can't tell which wire this is on: refuse rather than guess. */
        ESP_LOGW(TAG, "getsockname failed on an accepted socket: errno %d, rejecting", errno);
        return ESP_FAIL;
    }

    if (local.sin_family == AF_INET && local.sin_addr.s_addr == want.addr) {
        return ESP_OK;
    }

    /* Rejected. This runs on an accept path an attacker controls, and the dongle's only
     * console is a single shared UART — an unthrottled log here would let a connection flood
     * become a log flood. Same last_log idiom as relay_udp.c and relay_tcp.c. */
    static uint32_t last_log;
    uint32_t t = now_ms();
    if ((uint32_t)(t - last_log) > 1000) {
        last_log = t;
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);
        if (getpeername(sockfd, (struct sockaddr *)&peer, &peer_len) == 0 &&
            peer.sin_family == AF_INET) {
            esp_ip4_addr_t peer_ip = { .addr = peer.sin_addr.s_addr };
            ESP_LOGW(TAG, "rejected " IPSTR ":%u - connection did not land on DONGLE_HOST",
                     IP2STR(&peer_ip), (unsigned)ntohs(peer.sin_port));
        } else {
            ESP_LOGW(TAG, "rejected a connection not on DONGLE_HOST (peer unknown)");
        }
    }

    return ESP_FAIL;
}
