#include "relay_udp.h"

#include <errno.h>
#include <fcntl.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include "dongle_contract.inc"
#include "udp_sess.h"
#include "usb_net.h"
#include "wifi_sta.h"

static const char *TAG = "relay_udp";

/* The real-time channel's traffic — control frames, telemetry pushes — tops out far below
 * this; 1500 is a deliberate cap, not a guess at an MTU. recvfrom silently truncates a
 * datagram bigger than the buffer it is given and hands back no way to tell "fit exactly"
 * from "was cut off" — except this: give it one byte MORE room than the cap. lwIP's recvfrom
 * (lwip_recvfrom in sockets.c) returns min(buffer size, real datagram size), so a return of
 * exactly RELAY_BUF_LEN (1501) can only mean the real datagram was at least 1501 bytes — over
 * the 1500 cap — and is the truncation signal. Anything under that is the whole datagram.
 * Forwarding a truncated one is worse than dropping it: a half control frame parses as
 * something the app never sent, and the car has no way to tell the difference. */
#define RELAY_DATAGRAM_MAX 1500
#define RELAY_BUF_LEN (RELAY_DATAGRAM_MAX + 1)

/* select()'s timeout, and therefore the upper bound on how late an idle session's expiry can
 * run — one second, same as the brief asks, so a table full of abandoned sessions is noticed
 * even on a channel that has gone completely quiet. */
#define RELAY_LOOP_MS 1000

/* One task, so these can live at file scope instead of on the task's own stack — 4096 bytes
 * (this repo's precedent for a UDP relay task; see the car's rt_link.c) with two 1501-byte
 * buffers in inner scopes leaves the compiler's slot-sharing as the only thing standing
 * between select()/sendto()/ESP_LOGW's vprintf path and a stack overflow. Static removes the
 * question rather than trusting the optimisation. Never touched by more than one task, so no
 * lock is needed. */
static char s_phone_buf[RELAY_BUF_LEN];
static char s_car_buf[RELAY_BUF_LEN];

typedef struct {
    udp_sess_table_t sess;
    /* car_sock[i] mirrors sess.s[i].used exactly: valid (>=0) whenever, and only when, slot i
     * holds a live session. Kept exact even when opening a brand-new session's car-facing
     * socket fails — see handle_phone_datagram, which rolls the touch back rather than leave
     * a row marked used with no socket behind it. */
    int car_sock[UDP_SESS_MAX];
    int phone_sock;
    uint32_t gateway_be;   /* network byte order, meaningful once the wait loop below returns */
    uint32_t host_be;      /* DONGLE_HOST, parsed once; network byte order */
} relay_state_t;

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

/* Every socket this relay opens is made non-blocking, here, right after it is created —
 * before it is ever added to a select() set. Without this, a socket that select() marked
 * readable in one pass can be closed and its fd number handed straight back by lwIP's
 * alloc_socket (which scans sockets[] from 0 for the first free entry — so the fd just freed
 * by an eviction is the likely candidate) to a brand-new session before the next recv() on it
 * runs. A blocking recv() on that fresh socket — nothing queued for it yet — would then wait
 * forever: no forwarding, no expiry, no gateway poll, the whole relay wedged on one socket.
 * Non-blocking turns that into an EAGAIN/EWOULDBLOCK the existing error paths already handle,
 * rather than a hang. */
static bool set_nonblocking(int s)
{
    int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0 || fcntl(s, F_SETFL, flags | O_NONBLOCK) < 0) {
        ESP_LOGW(TAG, "fcntl O_NONBLOCK failed on fd %d: errno %d", s, errno);
        return false;
    }
    return true;
}

static void close_car_sock(relay_state_t *r, int idx)
{
    if (r->car_sock[idx] >= 0) {
        close(r->car_sock[idx]);
        r->car_sock[idx] = -1;
    }
}

static void close_all_car_socks(relay_state_t *r)
{
    for (int i = 0; i < UDP_SESS_MAX; i++) close_car_sock(r, i);
}

/* One ephemeral UDP socket, connect()ed to gateway:DONGLE_RELAY_RT_PORT. connect() on a
 * datagram socket sets a default destination for send() and, just as importantly, filters
 * what recv() will hand back — only datagrams from that exact address:port arrive on this
 * socket, so the socket a reply shows up on identifies the session with no lookup, per the
 * brief's "one socket per session". */
static int open_car_sock(uint32_t gateway_be, uint32_t host_be)
{
    /* The gateway comes from whatever network the dongle was told to join, so it is attacker-
     * influenced in the only sense that matters here: a network that advertises DONGLE_HOST
     * as its router makes this relay dial its own listener. lwIP short-circuits a packet
     * addressed to one of its own netif addresses into netif_loop_output, so the datagram
     * comes straight back to the phone-facing socket, is seen as arriving from a new peer,
     * and opens another session — one datagram becomes an unbounded loop that allocates a
     * session per iteration. Refused here rather than survived. Rate-limited: a phone
     * streaming at 10 Hz would otherwise put this on the sole UART console ten times a
     * second for the whole time such a network stays joined. */
    if (gateway_be == host_be) {
        static uint32_t last_log;
        uint32_t t = now_ms();
        if ((uint32_t)(t - last_log) > 1000) {
            last_log = t;
            ESP_LOGE(TAG, "refusing to relay to %s: the joined network names the dongle "
                          "itself as its gateway", DONGLE_HOST);
        }
        return -1;
    }

    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) {
        ESP_LOGW(TAG, "car-facing socket: errno %d", errno);
        return -1;
    }
    if (!set_nonblocking(s)) {
        close(s);
        return -1;
    }
    struct sockaddr_in car = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = gateway_be,
        .sin_port = htons(DONGLE_RELAY_RT_PORT),
    };
    if (connect(s, (struct sockaddr *)&car, sizeof(car)) < 0) {
        ESP_LOGW(TAG, "car-facing connect: errno %d", errno);
        close(s);
        return -1;
    }
    return s;
}

/* Phone -> car. The session's socket is created here on first sight of a new (address, port)
 * — "new" meaning udp_sess_find came back empty before this touch, whether because the peer
 * is genuinely new or because it just evicted someone else's slot. An existing session's
 * datagram just sends. */
static void handle_phone_datagram(relay_state_t *r, const char *buf, int n,
                                   const struct sockaddr_in *from)
{
    uint32_t addr = from->sin_addr.s_addr;
    uint16_t port = ntohs(from->sin_port);

    bool is_new = udp_sess_find(&r->sess, addr, port) < 0;
    int idx = udp_sess_touch(&r->sess, addr, port, now_ms());

    if (is_new) {
        /* A slot won by eviction can still carry the socket of the peer it displaced. */
        close_car_sock(r, idx);
        r->car_sock[idx] = open_car_sock(r->gateway_be, r->host_be);
        if (r->car_sock[idx] < 0) {
            /* Roll the touch back rather than leave the slot marked used with no socket
             * behind it: udp_sess.h has no "forget this one" call — a pure table has no
             * socket to fail opening — but its fields are plain and public, and undoing
             * exactly what touch() just did is enough to keep car_sock[] and sess.s[].used
             * from ever disagreeing. Without this, udp_sess_touch's unconditional call above
             * would keep refreshing this same broken slot's deadline for as long as this
             * peer kept sending, and it would never expire, never retry, and never let
             * another peer take the slot either. */
            r->sess.s[idx].used = false;
            ESP_LOGW(TAG, "dropping a datagram: no car-facing socket for slot %d", idx);
            return;
        }
    }

    if (send(r->car_sock[idx], buf, (size_t)n, 0) < 0) {
        /* Rate-limited: a Wi-Fi drop fails every send, and udp_sess_touch (above) refreshes
         * this session's deadline on every phone datagram regardless of whether the send that
         * follows succeeds — so the session cannot age out while the phone keeps streaming,
         * and an unthrottled log here would be one ESP_LOGW per datagram, at 10 Hz, on the
         * dongle's sole UART console, for the whole outage. Same idiom as rt_link.c's
         * last_log. */
        static uint32_t last_log;
        uint32_t t = now_ms();
        if ((uint32_t)(t - last_log) > 1000) {
            last_log = t;
            ESP_LOGW(TAG, "phone->car send failed on slot %d: errno %d", idx, errno);
        }
    }
}

/* Car -> phone. sendto, not send: the phone-facing socket is the one shared listener, bound
 * to DONGLE_HOST:DONGLE_RELAY_RT_PORT, so every reply must name which phone it is for — the
 * address and port udp_sess recorded for this slot when the session was created. */
static void handle_car_datagram(relay_state_t *r, int idx, const char *buf, int n)
{
    const udp_sess_t *s = &r->sess.s[idx];
    struct sockaddr_in to = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = s->addr,
        .sin_port = htons(s->port),
    };
    if (sendto(r->phone_sock, buf, (size_t)n, 0, (struct sockaddr *)&to, sizeof(to)) < 0) {
        /* Rate-limited for the same reason as the phone->car send above. */
        static uint32_t last_log;
        uint32_t t = now_ms();
        if ((uint32_t)(t - last_log) > 1000) {
            last_log = t;
            ESP_LOGW(TAG, "car->phone send failed on slot %d: errno %d", idx, errno);
        }
    }
}

static void expire_sessions(relay_state_t *r)
{
    uint32_t freed = udp_sess_expire(&r->sess, now_ms(), UDP_SESS_IDLE_MS);
    for (int i = 0; i < UDP_SESS_MAX; i++) {
        if (freed & (1u << i)) close_car_sock(r, i);
    }
}

/* The gateway moved. A session aimed at the old one is worse than no session — its car-facing
 * socket is connect()ed to an address that may no longer even be the car — so every one of
 * them goes, along with the pure table that named them; the next datagram from each phone
 * opens a fresh session against the new gateway. The phone-facing socket is untouched: it is
 * not aimed at the car at all, and the phone should not need to notice this happened. */
static void reaim(relay_state_t *r, uint32_t gateway_be)
{
    ESP_LOGW(TAG, "gateway changed — closing every car-facing session");
    close_all_car_socks(r);
    udp_sess_init(&r->sess);
    r->gateway_be = gateway_be;
}

static void relay_task(void *arg)
{
    (void)arg;
    relay_state_t r = { .phone_sock = -1, .gateway_be = 0, .host_be = 0 };
    udp_sess_init(&r.sess);
    for (int i = 0; i < UDP_SESS_MAX; i++) r.car_sock[i] = -1;

    /* Parsed first, before the gateway is even read: open_car_sock compares against it to
     * refuse a network that advertises the dongle itself as its gateway, and that comparison
     * has to be in place before the first datagram can create a session. usb_net_start()
     * (app_main, before this task can ever reach here) already gave the USB netif this exact
     * static address, so the only way this fails is a real configuration error. */
    esp_ip4_addr_t host_ip;
    if (esp_netif_str_to_ip4(DONGLE_HOST, &host_ip) != ESP_OK) {
        ESP_LOGE(TAG, "DONGLE_HOST does not parse as an address");
        vTaskDelete(NULL);
        return;
    }
    r.host_be = host_ip.addr;

    /* Nowhere to forward until the station has joined at least once. wifi_sta_gateway() is a
     * poll, not a callback — see wifi_sta.h's own comment on why one callback slot cannot
     * serve both this relay and Task 5's — so this is an ordinary retry loop, not a wait on
     * anything. */
    while (!wifi_sta_gateway(&r.gateway_be)) {
        vTaskDelay(pdMS_TO_TICKS(RELAY_LOOP_MS));
    }

    r.phone_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (r.phone_sock < 0) {
        ESP_LOGE(TAG, "phone-facing socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }
    if (!set_nonblocking(r.phone_sock)) {
        close(r.phone_sock);
        vTaskDelete(NULL);
        return;
    }

    /* THIS is what keeps the car's network from reaching the relay — not the bind below.
     * See usb_net.h: bind() sets the pcb's address, SO_BINDTODEVICE sets its interface, and
     * only the second one is an interface filter on a weak-host stack. Fail closed: a relay
     * that could not be pinned is one a station on the car's network can drive, and four
     * source ports at a low rate would hold the whole four-slot session table against the
     * phone. Refusing to serve at all is the honest outcome, and it is loud on the console
     * rather than silent. */
    if (usb_net_bind_socket(r.phone_sock) != ESP_OK) {
        ESP_LOGE(TAG, "cannot pin the real-time relay to the USB wire — not serving it");
        close(r.phone_sock);
        vTaskDelete(NULL);
        return;
    }

    /* Bound to DONGLE_HOST rather than INADDR_ANY as well. That is not the isolation (the
     * pin above is); it is what makes every reply leave with the address the phone sent to,
     * so a phone's socket accepts it. */
    struct sockaddr_in phone_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(DONGLE_RELAY_RT_PORT),
        .sin_addr.s_addr = r.host_be,
    };
    if (bind(r.phone_sock, (struct sockaddr *)&phone_addr, sizeof(phone_addr)) < 0) {
        ESP_LOGE(TAG, "phone-facing bind %s:%d: errno %d", DONGLE_HOST, DONGLE_RELAY_RT_PORT,
                 errno);
        close(r.phone_sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "real-time relay up on %s:%d", DONGLE_HOST, DONGLE_RELAY_RT_PORT);

    for (;;) {
        /* Polled once per pass, per the task brief's correction: there is no connected
         * callback, and this loop already wakes at least once a second. */
        uint32_t gw;
        if (wifi_sta_gateway(&gw) && gw != r.gateway_be) {
            reaim(&r, gw);
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(r.phone_sock, &rfds);
        int maxfd = r.phone_sock;
        for (int i = 0; i < UDP_SESS_MAX; i++) {
            if (r.car_sock[i] >= 0) {
                FD_SET(r.car_sock[i], &rfds);
                if (r.car_sock[i] > maxfd) maxfd = r.car_sock[i];
            }
        }

        struct timeval tv = { .tv_sec = RELAY_LOOP_MS / 1000, .tv_usec = 0 };
        int nready = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (nready > 0) {
            if (FD_ISSET(r.phone_sock, &rfds)) {
                struct sockaddr_in from;
                socklen_t flen = sizeof(from);
                /* Non-blocking (set_nonblocking, above): a stale readiness bit — e.g. this
                 * fd was reused by an eviction after select() sampled it but before this line
                 * runs — costs an EAGAIN here, not a wait. */
                int n = recvfrom(r.phone_sock, s_phone_buf, sizeof(s_phone_buf), 0,
                                  (struct sockaddr *)&from, &flen);
                if (n == RELAY_BUF_LEN) {
                    ESP_LOGW(TAG, "phone->car datagram over %d bytes, dropped whole",
                              RELAY_DATAGRAM_MAX);
                } else if (n > 0 && from.sin_family == AF_INET) {
                    handle_phone_datagram(&r, s_phone_buf, n, &from);
                } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                    ESP_LOGW(TAG, "phone-facing recvfrom: errno %d", errno);
                }
            }
            for (int i = 0; i < UDP_SESS_MAX; i++) {
                if (r.car_sock[i] < 0 || !FD_ISSET(r.car_sock[i], &rfds)) continue;
                /* Same non-blocking guarantee as the phone-facing read above. */
                int n = recv(r.car_sock[i], s_car_buf, sizeof(s_car_buf), 0);
                if (n == RELAY_BUF_LEN) {
                    ESP_LOGW(TAG, "car->phone datagram over %d bytes, dropped whole (slot %d)",
                              RELAY_DATAGRAM_MAX, i);
                } else if (n > 0) {
                    handle_car_datagram(&r, i, s_car_buf, n);
                } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                    ESP_LOGW(TAG, "car-facing recv (slot %d): errno %d", i, errno);
                }
            }
        } else if (nready < 0 && errno != EINTR) {
            ESP_LOGW(TAG, "select: errno %d", errno);
        }

        expire_sessions(&r);
    }
}

esp_err_t relay_udp_start(void)
{
    if (xTaskCreate(relay_task, "relay_udp", 4096, NULL, 5, NULL) != pdPASS) {
        return ESP_FAIL;
    }
    return ESP_OK;
}
