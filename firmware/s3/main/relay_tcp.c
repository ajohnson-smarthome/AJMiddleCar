#include "relay_tcp.h"

#include <errno.h>
#include <fcntl.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include "dongle_contract.inc"
#include "wifi_sta.h"

static const char *TAG = "relay_tcp";

/* Pool size is a design number, not a detail — the spec says so. Four: the app can have a
 * config POST and a firmware upload in flight at once, and a pool of one would deadlock the
 * second behind the first. Four leaves room for the browser-style parallelism a REST client
 * may use without letting a leaked slot starve the pool. */
#define RELAY_POOL_SIZE 4

/* TCP is a byte stream, not a datagram: unlike relay_udp.c's RELAY_BUF_LEN, this needs no
 * "+1 to detect truncation" trick. A recv() that does not drain everything queued just
 * leaves the remainder queued for the next pass — nothing is lost at a buffer boundary the
 * way a UDP datagram would be. 1460 bytes per the brief. */
#define RELAY_BUF_LEN 1460

/* select()'s timeout, and therefore the cadence of the gateway poll below — one second,
 * same rhythm as relay_udp.c. */
#define RELAY_LOOP_MS 1000

/* How patiently pump_send (below) waits out a destination whose own TCP send buffer is
 * momentarily full, and how often it checks back. Not a guess at a "correct" number: a
 * destination that never drains within a second is treated as dead, same order of magnitude
 * as RELAY_LOOP_MS. */
#define RELAY_DRAIN_TIMEOUT_MS 1000
#define RELAY_DRAIN_RETRY_MS 10

/* One task, so these can live at file scope instead of the task's own stack — see
 * relay_udp.c's identical comment for why that is a requirement here and not merely tidy.
 * Two buffers, not eight: one task pumps one direction of one slot at a time, so a shared
 * buffer per direction is enough regardless of how many slots are live. */
static char s_phone_buf[RELAY_BUF_LEN];
static char s_car_buf[RELAY_BUF_LEN];

typedef enum {
    SLOT_FREE = 0,     /* no connection; both sockets -1 */
    SLOT_CONNECTING,   /* accepted from the phone, car_sock's connect() not yet resolved */
    SLOT_ACTIVE,       /* both sides open; pumped in both directions */
} slot_state_t;

typedef struct {
    slot_state_t state;
    int phone_sock;
    int car_sock;
} tcp_slot_t;

typedef struct {
    tcp_slot_t slots[RELAY_POOL_SIZE];
    int listen_sock;
    uint32_t gateway_be;   /* network byte order, meaningful once the wait loop below returns */
} relay_state_t;

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

/* Every socket this relay opens is made non-blocking, here, right after it is created —
 * before it is ever added to a select() set. See relay_udp.c's set_nonblocking for the fd-
 * reuse hazard this avoids: the same rhythm (a slot freed by one peer's close, a new accept
 * in the same pass) applies here too, and lwIP's alloc_socket can hand the freed fd straight
 * back. Non-blocking turns a stale readiness bit into EAGAIN/EWOULDBLOCK, not a hang. */
static bool set_nonblocking(int s)
{
    int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0 || fcntl(s, F_SETFL, flags | O_NONBLOCK) < 0) {
        ESP_LOGW(TAG, "fcntl O_NONBLOCK failed on fd %d: errno %d", s, errno);
        return false;
    }
    return true;
}

/* Closes both sockets of a slot (whatever state it is in — FREE is a harmless no-op since
 * both are already -1) and frees it. The single exit for every "this connection is over"
 * path in this file: no half-close. Either half returning 0 or an error takes this path for
 * both sockets together, never one alone — the car's REST surface has no use for a half-open
 * connection, and closing only one leaks the slot's other socket forever. */
static void close_slot(tcp_slot_t *slots, int idx, const char *why)
{
    tcp_slot_t *s = &slots[idx];
    if (s->state == SLOT_FREE) {
        return;
    }
    if (s->phone_sock >= 0) close(s->phone_sock);
    if (s->car_sock >= 0) close(s->car_sock);
    s->phone_sock = -1;
    s->car_sock = -1;
    s->state = SLOT_FREE;
    /* Unlike relay_udp.c's per-datagram sends, a TCP connection's close is a rare event —
     * on the order of one per REST request, not one per 10 Hz frame — so this is logged
     * plainly every time rather than rate-limited. */
    ESP_LOGI(TAG, "slot %d closed: %s", idx, why);
}

/* Sends every one of the len bytes at buf to dst, waiting out ordinary backpressure — the
 * destination's own TCP send buffer momentarily full — rather than corrupting the stream by
 * forwarding only part of it. A short config POST or status GET almost never hits this: with
 * CONFIG_LWIP_TCP_SND_BUF_DEFAULT at 5760 bytes (firmware/s3/sdkconfig), a single 1460-byte
 * chunk fits easily. A firmware upload — the reason the pool is 4, not 1 — routinely does:
 * the phone-facing side (USB) can hand over chunks far faster than the car-facing side
 * (the softAP's own WiFi) can drain them, so the send buffer fills within a few passes of a
 * sustained transfer. Treating that as a hard error would make every large upload fail
 * partway through, which is worse than the bounded wait below.
 *
 * Never calls a blocking socket op — dst stays O_NONBLOCK throughout, and the wait between
 * attempts is vTaskDelay (a scheduler yield), not a blocking send(). That bounds a stuck
 * destination's cost to RELAY_DRAIN_TIMEOUT_MS of this task's attention, not an unbounded
 * one — the same "must not stall the other sessions" property the brief asks of the upstream
 * connect(), extended to the steady-state pump. A destination that has not accepted another
 * byte in that long is treated the same as any other error: the slot is closed. */
static bool pump_send(int dst, const char *buf, int len)
{
    int off = 0;
    uint32_t start = now_ms();
    while (off < len) {
        int w = send(dst, buf + off, (size_t)(len - off), 0);
        if (w > 0) {
            off += w;
            continue;
        }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if ((uint32_t)(now_ms() - start) >= RELAY_DRAIN_TIMEOUT_MS) {
                return false;
            }
            vTaskDelay(pdMS_TO_TICKS(RELAY_DRAIN_RETRY_MS));
            continue;
        }
        return false;   /* w == 0 (shouldn't happen for send(), but not a success either) or
                          * a real error — either way, not this function's job to log; the
                          * caller knows which direction and which slot. */
    }
    return true;
}

/* One direction of one slot: read what is waiting on src, forward every byte of it to dst.
 * label is purely for the log line ("phone->car" or "car->phone"). Closes the slot on a
 * clean EOF, a real recv() error, or a forwarding failure — including the backpressure
 * timeout inside pump_send — and leaves it alone on EAGAIN (nothing to do this pass) exactly
 * like relay_udp.c's read paths. */
static void pump_direction(tcp_slot_t *slots, int idx, int src, int dst, char *buf,
                            const char *label)
{
    int n = recv(src, buf, RELAY_BUF_LEN, 0);
    if (n > 0) {
        if (!pump_send(dst, buf, n)) {
            ESP_LOGW(TAG, "%s forwarding failed on slot %d: errno %d", label, idx, errno);
            close_slot(slots, idx, "forwarding failed");
        }
        return;
    }
    if (n == 0) {
        close_slot(slots, idx, "peer closed");
        return;
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
        ESP_LOGW(TAG, "%s recv failed on slot %d: errno %d", label, idx, errno);
        close_slot(slots, idx, "recv error");
    }
}

/* The gateway moved. Every live slot's car-facing socket is connected to the OLD gateway —
 * possibly not even the car any more — so all of them go, the same call relay_udp.c makes
 * for its sessions. The listener is untouched: it is not aimed at the car at all, and a
 * phone mid-request should not need to notice this happened (its connection to the OLD
 * upstream is gone either way, and it will simply see the connection drop and retry). */
static void reaim(relay_state_t *r, uint32_t gateway_be)
{
    ESP_LOGW(TAG, "gateway changed — closing every live REST session");
    for (int i = 0; i < RELAY_POOL_SIZE; i++) {
        if (r->slots[i].state != SLOT_FREE) {
            close_slot(r->slots, i, "gateway changed");
        }
    }
    r->gateway_be = gateway_be;
}

/* Accept exactly one connection (the caller already knows the listener is readable) and, if
 * a slot is free, open the matching upstream connection. On any failure past accept() itself,
 * every socket this function opened is closed before returning — there is no path here that
 * leaves an fd behind uncounted. */
static void handle_accept(relay_state_t *r)
{
    struct sockaddr_in from;
    socklen_t flen = sizeof(from);
    int c = accept(r->listen_sock, (struct sockaddr *)&from, &flen);
    if (c < 0) {
        /* Non-blocking (set_nonblocking on the listener, below): a stale readiness bit costs
         * an EAGAIN here, not a wait — same guarantee as every recv() in this file. */
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            ESP_LOGW(TAG, "accept: errno %d", errno);
        }
        return;
    }
    if (!set_nonblocking(c)) {
        close(c);
        return;
    }

    int idx = -1;
    for (int i = 0; i < RELAY_POOL_SIZE; i++) {
        if (r->slots[i].state == SLOT_FREE) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        /* Step 4 of the brief: close immediately rather than queue. A REST client sees a
         * refused connection and retries; a client held open by a relay with no capacity
         * sees a hang instead — worse, and harder to diagnose. */
        ESP_LOGW(TAG, "REST relay pool full (%d slots) — refusing a connection",
                 RELAY_POOL_SIZE);
        close(c);
        return;
    }

    int car = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (car < 0) {
        ESP_LOGW(TAG, "upstream socket: errno %d", errno);
        close(c);
        return;
    }
    if (!set_nonblocking(car)) {
        close(car);
        close(c);
        return;
    }

    /* The upstream connect() must not block the task: the socket is already non-blocking, so
     * this either completes immediately, or returns EINPROGRESS and is finished later,
     * through the same select() loop that watches every other socket — completion is
     * observed as car_sock becoming writable (see the SLOT_CONNECTING handling in
     * relay_task). A car that is powered but slow to answer therefore costs this one slot,
     * never the other three. */
    struct sockaddr_in dst = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = r->gateway_be,
        .sin_port = htons(DONGLE_RELAY_HTTP_PORT),
    };
    int cr = connect(car, (struct sockaddr *)&dst, sizeof(dst));
    if (cr == 0) {
        /* Completed synchronously. POSIX allows this even for a non-blocking socket (most
         * often seen on a loopback-fast path); lwIP does not special-case it away, so this
         * has to be handled rather than assumed impossible. */
        r->slots[idx].state = SLOT_ACTIVE;
    } else if (errno == EINPROGRESS) {
        r->slots[idx].state = SLOT_CONNECTING;
    } else {
        ESP_LOGW(TAG, "upstream connect: errno %d", errno);
        close(car);
        close(c);
        return;
    }
    r->slots[idx].phone_sock = c;
    r->slots[idx].car_sock = car;
}

static void relay_task(void *arg)
{
    (void)arg;
    relay_state_t r = { .listen_sock = -1, .gateway_be = 0 };
    for (int i = 0; i < RELAY_POOL_SIZE; i++) {
        r.slots[i] = (tcp_slot_t){ .state = SLOT_FREE, .phone_sock = -1, .car_sock = -1 };
    }

    /* Nowhere to forward until the station has joined at least once — same wait as
     * relay_udp.c, for the same reason: wifi_sta_gateway() is a poll, not a callback. */
    while (!wifi_sta_gateway(&r.gateway_be)) {
        vTaskDelay(pdMS_TO_TICKS(RELAY_LOOP_MS));
    }

    r.listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (r.listen_sock < 0) {
        ESP_LOGE(TAG, "listener socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }
    if (!set_nonblocking(r.listen_sock)) {
        close(r.listen_sock);
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in listen_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(DONGLE_RELAY_HTTP_PORT),
    };
    esp_ip4_addr_t host_ip;
    /* Bind to DONGLE_HOST specifically, not INADDR_ANY — see relay_tcp.h. This is what
     * keeps the car's side of the dongle from ever reaching this listener. usb_net_start()
     * (app_main, before this task can ever reach here) already gave the USB netif this exact
     * static address, so the only way this fails is a real configuration error, not a timing
     * race against the interface coming up. */
    if (esp_netif_str_to_ip4(DONGLE_HOST, &host_ip) != ESP_OK) {
        ESP_LOGE(TAG, "DONGLE_HOST does not parse as an address");
        close(r.listen_sock);
        vTaskDelete(NULL);
        return;
    }
    listen_addr.sin_addr.s_addr = host_ip.addr;
    if (bind(r.listen_sock, (struct sockaddr *)&listen_addr, sizeof(listen_addr)) < 0) {
        ESP_LOGE(TAG, "listener bind %s:%d: errno %d", DONGLE_HOST, DONGLE_RELAY_HTTP_PORT,
                 errno);
        close(r.listen_sock);
        vTaskDelete(NULL);
        return;
    }
    if (listen(r.listen_sock, RELAY_POOL_SIZE) < 0) {
        ESP_LOGE(TAG, "listen: errno %d", errno);
        close(r.listen_sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "REST relay up on %s:%d", DONGLE_HOST, DONGLE_RELAY_HTTP_PORT);

    for (;;) {
        /* Polled once per pass, per the task brief's correction: there is no connected
         * callback, and this loop already wakes at least once a second. */
        uint32_t gw;
        if (wifi_sta_gateway(&gw) && gw != r.gateway_be) {
            reaim(&r, gw);
        }

        fd_set rfds, wfds;
        FD_ZERO(&rfds);
        FD_ZERO(&wfds);
        FD_SET(r.listen_sock, &rfds);
        int maxfd = r.listen_sock;
        for (int i = 0; i < RELAY_POOL_SIZE; i++) {
            tcp_slot_t *s = &r.slots[i];
            if (s->state == SLOT_ACTIVE) {
                FD_SET(s->phone_sock, &rfds);
                if (s->phone_sock > maxfd) maxfd = s->phone_sock;
                FD_SET(s->car_sock, &rfds);
                if (s->car_sock > maxfd) maxfd = s->car_sock;
            } else if (s->state == SLOT_CONNECTING) {
                /* connect() completion is observed as writability, not readability — see
                 * handle_accept's comment. */
                FD_SET(s->car_sock, &wfds);
                if (s->car_sock > maxfd) maxfd = s->car_sock;
            }
        }

        struct timeval tv = { .tv_sec = RELAY_LOOP_MS / 1000, .tv_usec = 0 };
        int nready = select(maxfd + 1, &rfds, &wfds, NULL, &tv);
        if (nready > 0) {
            if (FD_ISSET(r.listen_sock, &rfds)) {
                handle_accept(&r);
            }

            for (int i = 0; i < RELAY_POOL_SIZE; i++) {
                tcp_slot_t *s = &r.slots[i];

                if (s->state == SLOT_CONNECTING && FD_ISSET(s->car_sock, &wfds)) {
                    int err = 0;
                    socklen_t elen = sizeof(err);
                    if (getsockopt(s->car_sock, SOL_SOCKET, SO_ERROR, &err, &elen) < 0) {
                        ESP_LOGW(TAG, "upstream connect: getsockopt failed (slot %d): errno %d",
                                 i, errno);
                        close_slot(r.slots, i, "connect failed");
                    } else if (err != 0) {
                        ESP_LOGW(TAG, "upstream connect failed (slot %d): errno %d", i, err);
                        close_slot(r.slots, i, "connect failed");
                    } else {
                        s->state = SLOT_ACTIVE;
                    }
                    /* Either way, nothing queued to pump on this slot yet this pass — a
                     * freshly-connected socket has no data waiting, and a just-closed one
                     * has no sockets left to read. */
                    continue;
                }

                if (s->state != SLOT_ACTIVE) {
                    continue;
                }
                /* Non-blocking (set_nonblocking on both sockets, at creation): a stale
                 * readiness bit here — this slot's fd reused by an eviction after select()
                 * sampled it — costs an EAGAIN inside pump_direction, not a wait. */
                if (FD_ISSET(s->phone_sock, &rfds)) {
                    pump_direction(r.slots, i, s->phone_sock, s->car_sock, s_phone_buf,
                                   "phone->car");
                }
                /* Re-check state: the phone->car pump just above may have closed this same
                 * slot (a forwarding failure, or the phone hanging up), in which case
                 * car_sock is already -1 and there is nothing left to read from it. */
                if (s->state == SLOT_ACTIVE && FD_ISSET(s->car_sock, &rfds)) {
                    pump_direction(r.slots, i, s->car_sock, s->phone_sock, s_car_buf,
                                   "car->phone");
                }
            }
        } else if (nready < 0 && errno != EINTR) {
            ESP_LOGW(TAG, "select: errno %d", errno);
        }
    }
}

esp_err_t relay_tcp_start(void)
{
    if (xTaskCreate(relay_task, "relay_tcp", 4096, NULL, 5, NULL) != pdPASS) {
        return ESP_FAIL;
    }
    return ESP_OK;
}
