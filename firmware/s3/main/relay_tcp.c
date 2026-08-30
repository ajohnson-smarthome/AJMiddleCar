#include "relay_tcp.h"

#include <errno.h>
#include <fcntl.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include "dongle_contract.inc"
#include "tcp_pending.h"
#include "usb_net.h"
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
 * way a UDP datagram would be. 1460 bytes per the brief — the same 1460 as tcp_pending.h's
 * TCP_PENDING_BUF_LEN, on purpose: a backlog can never need to hold more than one read
 * produced in a single shot, so the two are defined in terms of each other rather than as
 * two numbers that happen to agree today. */
#define RELAY_BUF_LEN TCP_PENDING_BUF_LEN

/* select()'s timeout, and therefore the cadence of the gateway poll and the connect-timeout
 * check below — one second, same rhythm as relay_udp.c. */
#define RELAY_LOOP_MS 1000

/* How long a SLOT_CONNECTING slot is allowed to wait for the upstream connect() to resolve
 * before this relay gives up on it. Sized against a station that is already associated to
 * the car's softAP (relay_task waits for wifi_sta_gateway() before it ever opens the
 * listener, so an *initial* association is never on this clock) but that could have dropped
 * and be silently re-associating at the exact moment a phone's connection arrives — plus an
 * ordinary TCP handshake's round trips. A few seconds covers that with room to spare; it is
 * nowhere near CONFIG_LWIP_TCP_SYNMAXRTX's (12) multi-minute worst case, which is exactly
 * the point — see relay_tcp.h and the task-5 report for why that worst case must not be
 * allowed to pin a slot. */
#define RELAY_CONNECT_TIMEOUT_MS 5000

/* One task, so the receive scratch buffers can live at file scope instead of the task's own
 * stack — see relay_udp.c's identical comment for why that is a requirement here and not
 * merely tidy. Two of them, not eight: one task reads one direction of one slot at a time, so
 * a single shared scratch buffer per direction is enough regardless of how many slots are
 * live — the brief's "reused per pass, not per slot" holds for these exactly as written; a
 * chunk is copied out of one of these before the next recv() into it. */
static char s_phone_buf[RELAY_BUF_LEN];
static char s_car_buf[RELAY_BUF_LEN];

/* Pending backlogs are the one place that "reused per pass, not per slot" rule does not
 * apply, and deliberately so: when a destination's own TCP send buffer is momentarily full,
 * pump_read (below) cannot forward the whole chunk it just read, and the unsent remainder has
 * to live somewhere that survives past this pass — and past whatever OTHER slot this task
 * services next, since it is still the same single task working through all four. That makes
 * a backlog inherently per slot AND per direction, not a shared scratch — a different rule
 * for a different job, not a violation of the first one. File-scope statics for the same
 * stack-safety reason as the scratch buffers above. 4 slots × 2 directions ×
 * sizeof(tcp_pending_t) is 11,744 bytes (~11.47 KiB) of static RAM — see the task-5 report
 * for the arithmetic — comfortable against the S3's 512 KiB of internal SRAM. The bookkeeping
 * itself (stash a chunk, advance on partial progress, know when empty) lives in
 * tcp_pending.{c,h} as a pure, host-tested module; this file only owns the sockets around
 * it. */
static tcp_pending_t s_p2c_pending[RELAY_POOL_SIZE];   /* phone -> car backlog, by slot */
static tcp_pending_t s_c2p_pending[RELAY_POOL_SIZE];   /* car -> phone backlog, by slot */

typedef enum {
    SLOT_FREE = 0,     /* no connection; both sockets -1 */
    SLOT_CONNECTING,   /* accepted from the phone, car_sock's connect() not yet resolved */
    SLOT_ACTIVE,       /* both sides open; pumped in both directions */
} slot_state_t;

typedef struct {
    slot_state_t state;
    int phone_sock;
    int car_sock;
    uint32_t connect_started_ms;    /* meaningful only while state == SLOT_CONNECTING */
} tcp_slot_t;

typedef struct {
    tcp_slot_t slots[RELAY_POOL_SIZE];
    int listen_sock;
    uint32_t gateway_be;   /* network byte order, meaningful once the wait loop below returns */
    uint32_t host_be;      /* DONGLE_HOST, parsed once; network byte order */
} relay_state_t;

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

/* Every socket this relay opens is made non-blocking, here, right after it is created —
 * before it is ever added to a select() set. This file's design depends on it directly:
 * SLOT_CONNECTING's connect() only works non-blocking, and flush_pending/pump_read's whole
 * backpressure scheme is built on recv()/send() returning EAGAIN/EWOULDBLOCK rather than
 * blocking when there is nothing to do right now. It is also defense in depth against the
 * specific hazard relay_udp.c has to survive (a slot freed and a new accept landing on the
 * same fd number within one pass) — though see the per-slot loop below for why that
 * particular scenario cannot actually arise in this file's control flow. */
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
 * connection, and closing only one leaks the slot's other socket forever.
 *
 * Takes the whole relay_state_t, not a bare tcp_slot_t array, so the coupling to this slot's
 * pending backlogs below is structural rather than a raw idx into file-scope arrays that
 * merely happen to be indexed the same way reaim, handle_accept and every other stateful
 * function in this file take relay_state_t for the same reason. */
static void close_slot(relay_state_t *r, int idx, const char *why)
{
    tcp_slot_t *s = &r->slots[idx];
    if (s->state == SLOT_FREE) {
        return;
    }
    if (s->phone_sock >= 0) close(s->phone_sock);
    if (s->car_sock >= 0) close(s->car_sock);
    s->phone_sock = -1;
    s->car_sock = -1;
    s->state = SLOT_FREE;
    /* A slot's pending backlog belongs to the connection that just ended; clear both
     * directions so a future occupant of this same index starts empty rather than inheriting
     * — and eventually flushing — a stranger's leftover bytes. */
    tcp_pending_clear(&s_p2c_pending[idx]);
    tcp_pending_clear(&s_c2p_pending[idx]);
    /* Unlike relay_udp.c's per-datagram sends, a TCP connection's close is a rare event —
     * on the order of one per REST request, not one per 10 Hz frame — so this is logged
     * plainly every time rather than rate-limited. */
    ESP_LOGI(TAG, "slot %d closed: %s", idx, why);
}

/* Sends as much of a slot's already-buffered backlog as dst currently accepts. Called only
 * when select() has just reported dst writable for this direction, which only happens while
 * the direction actually has something pending (see relay_task's fd-set build) — so
 * !tcp_pending_empty(p) is guaranteed here. Makes exactly one send() attempt: whatever dst
 * still cannot take simply stays pending, and dst stays in next pass's writefds until
 * select() says it is worth trying again. No retry loop and no delay — an earlier version of
 * this file spun on EAGAIN with a bounded wait, which blocked this single task's attention
 * (and so every other slot) for up to a second at a time; this version never calls send()
 * more than once without a fresh select() saying to. */
static void flush_pending(relay_state_t *r, int idx, int dst, tcp_pending_t *p,
                           const char *label)
{
    int remaining = p->len - p->off;
    int w = send(dst, p->buf + p->off, (size_t)remaining, 0);
    if (w > 0) {
        tcp_pending_advance(p, w);
        return;
    }
    if (w == 0 || (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))) {
        /* No progress this attempt — not an error. lwIP never actually returns 0 for a
         * non-zero-length non-blocking send(), so the w == 0 half of this is unreachable in
         * practice; it is folded in here rather than treated as fatal so this function agrees
         * with pump_read's identical treatment of the same (unreachable) case, instead of the
         * two silently disagreeing about what a send() returning 0 would mean. The backlog
         * stays; select() will say so again next pass if dst is still not ready. */
        return;
    }
    ESP_LOGW(TAG, "%s forwarding failed on slot %d: errno %d", label, idx, errno);
    close_slot(r, idx, "forwarding failed");
}

/* One direction of one slot: called only when its source is in this pass's readfds, which
 * relay_task's fd-set build offers only while this direction's backlog (p) is empty — a call
 * here never needs to check for one first. Reads once into the shared per-direction scratch
 * buffer and makes exactly one send() attempt to forward it. Whatever dst does not accept
 * right now becomes this slot's pending backlog for the direction; relay_task's next fd-set
 * build then drops src from readfds and adds dst to writefds until flush_pending (above)
 * drains it. That is the whole backpressure mechanism: select() is what throttles a fast
 * producer against a slow consumer, not a retry loop in here, and a source socket is simply
 * never offered for reading again until its own backlog is gone — so this task can never read
 * faster than it can deliver.
 *
 * label is purely for the log line ("phone->car" or "car->phone"). Closes the slot on a clean
 * EOF, a real recv() error, or a forwarding failure, and leaves it alone on EAGAIN (nothing to
 * do this pass) exactly like relay_udp.c's read paths. */
static void pump_read(relay_state_t *r, int idx, int src, int dst, char *scratch,
                       tcp_pending_t *p, const char *label)
{
    int n = recv(src, scratch, RELAY_BUF_LEN, 0);
    if (n > 0) {
        int w = send(dst, scratch, (size_t)n, 0);
        if (w == n) {
            return;   /* the common case: forwarded whole, nothing left pending */
        }
        if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            ESP_LOGW(TAG, "%s forwarding failed on slot %d: errno %d", label, idx, errno);
            close_slot(r, idx, "forwarding failed");
            return;
        }
        /* Partial (0 <= w < n) or nothing accepted at all (w < 0, EAGAIN, or the same
         * unreachable-in-practice w == 0 flush_pending's comment describes) — either way not
         * an error: a fast producer outrunning a slow consumer is the ordinary condition this
         * backlog exists for, not something to log. Stash the unsent remainder; the fd-set
         * build (relay_task) does the actual throttling from here. */
        int sent = (w > 0) ? w : 0;
        tcp_pending_stash(p, scratch, n, sent);
        return;
    }
    if (n == 0) {
        close_slot(r, idx, "peer closed");
        return;
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
        ESP_LOGW(TAG, "%s recv failed on slot %d: errno %d", label, idx, errno);
        close_slot(r, idx, "recv error");
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
            close_slot(r, i, "gateway changed");
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
         * an EAGAIN here, not a wait — same guarantee as every recv() in this file. Anything
         * else — most plausibly the socket table itself being exhausted — repeats every pass
         * for as long as a client keeps retrying, so it is rate-limited like relay_udp.c's
         * per-datagram warnings rather than left to flood the sole UART console. */
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            static uint32_t last_log;
            uint32_t t = now_ms();
            if ((uint32_t)(t - last_log) > 1000) {
                last_log = t;
                ESP_LOGW(TAG, "accept: errno %d", errno);
            }
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
         * sees a hang instead — worse, and harder to diagnose. Rate-limited: a client
         * retrying against a pool that stays genuinely full hits this every pass. */
        static uint32_t last_log;
        uint32_t t = now_ms();
        if ((uint32_t)(t - last_log) > 1000) {
            last_log = t;
            ESP_LOGW(TAG, "REST relay pool full (%d slots) — refusing a connection",
                     RELAY_POOL_SIZE);
        }
        close(c);
        return;
    }

    /* Guards the connect() below, checked before a single upstream fd is spent on it. The
     * gateway is whatever network the dongle was told to join, and one that advertises
     * DONGLE_HOST as its router would aim this relay at its own listener: lwIP short-circuits
     * a packet addressed to one of its netifs' own addresses into netif_loop_output, so every
     * connection would be accepted straight back into this pool. One phone connection is then
     * enough to fill all four slots with self-connections and take the car's REST surface
     * down. Refused here rather than survived. Rate-limited: a REST client that retries hits
     * this on every attempt for as long as such a network stays joined. */
    if (r->gateway_be == r->host_be) {
        static uint32_t last_log;
        uint32_t t = now_ms();
        if ((uint32_t)(t - last_log) > 1000) {
            last_log = t;
            ESP_LOGE(TAG, "refusing to relay to %s: the joined network names the dongle "
                          "itself as its gateway", DONGLE_HOST);
        }
        close(c);
        return;
    }

    int car = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (car < 0) {
        /* Rate-limited for the same reason as the accept() failure above: a genuinely
         * exhausted socket table fails this on every accepted connection a retrying client
         * sends. */
        static uint32_t last_log;
        uint32_t t = now_ms();
        if ((uint32_t)(t - last_log) > 1000) {
            last_log = t;
            ESP_LOGW(TAG, "upstream socket: errno %d", errno);
        }
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
     * never the other three — bounded further by RELAY_CONNECT_TIMEOUT_MS below, since a
     * car that is off rather than merely slow can otherwise pin this slot for the several
     * minutes CONFIG_LWIP_TCP_SYNMAXRTX's retry budget allows. */
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
        r->slots[idx].connect_started_ms = now_ms();
    } else {
        /* Rate-limited: a car actively refusing the port (ECONNREFUSED comes back fast, no
         * SYN retries involved) fails this on every attempt a retrying client makes. */
        static uint32_t last_log;
        uint32_t t = now_ms();
        if ((uint32_t)(t - last_log) > 1000) {
            last_log = t;
            ESP_LOGW(TAG, "upstream connect: errno %d", errno);
        }
        close(car);
        close(c);
        return;
    }
    r->slots[idx].phone_sock = c;
    r->slots[idx].car_sock = car;
}

/* One SLOT_CONNECTING slot, checked every pass regardless of what select() returned. Three
 * things can end a connect attempt, and Step 6 of the brief ("either half returning 0 or an
 * error closes both and frees the slot") is not scoped to SLOT_ACTIVE — it applies here too:
 *
 * 1. The deadline. Checked first and unconditionally, because a car that never answers at
 *    all — the ordinary failure mode of "the car is off", not an exotic one — leaves
 *    select() timing out with nothing ready, pass after pass; only a check that runs whether
 *    or not select() found anything can ever catch that.
 * 2. The phone hanging up while this relay is still waiting on the car. Watched via
 *    MSG_PEEK, not a real recv(): a phone that has already sent its request (an ordinary
 *    thing to do right after connect(), without waiting for anything from the far end) must
 *    not have those bytes silently discarded here — there is nowhere to stash them, since
 *    this slot has no pending backlog until it is ACTIVE. MSG_PEEK reports EOF/error without
 *    consuming a live request, leaving it queued for pump_read once this slot goes ACTIVE.
 * 3. The connect() itself resolving, via the SO_ERROR/writable check the brief describes.
 *
 * Any of the three can close the slot; each returns immediately after doing so rather than
 * falling into the next check on a slot that no longer exists. */
static void handle_connecting(relay_state_t *r, int i, bool had_ready, fd_set *rfds,
                               fd_set *wfds)
{
    tcp_slot_t *s = &r->slots[i];

    if ((uint32_t)(now_ms() - s->connect_started_ms) >= RELAY_CONNECT_TIMEOUT_MS) {
        /* Same wraparound-safe elapsed-time idiom as udp_sess_expire's: this subtraction
         * elapses correctly even across the millisecond counter's ~49.7-day wrap. */
        ESP_LOGW(TAG, "upstream connect timed out (slot %d)", i);
        close_slot(r, i, "connect timed out");
        return;
    }

    if (had_ready && FD_ISSET(s->phone_sock, rfds)) {
        char peek;
        int n = recv(s->phone_sock, &peek, sizeof(peek), MSG_PEEK);
        if (n == 0) {
            close_slot(r, i, "phone closed during connect");
            return;
        }
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            /* Mirrors pump_read's "recv failed" path: a real error, not an ordinary hangup,
             * so the errno is worth keeping — this whole connect-in-progress window is the
             * least-proven part of the file. */
            ESP_LOGW(TAG, "phone recv during connect failed (slot %d): errno %d", i, errno);
            close_slot(r, i, "phone closed during connect");
            return;
        }
        /* n > 0: a real request byte is queued and MSG_PEEK left it untouched in the kernel
         * buffer for pump_read to read for real once this slot goes ACTIVE. n < 0 with
         * EAGAIN: a spurious wakeup: nothing to do. Either way, keep waiting on the car. */
    }

    if (had_ready && FD_ISSET(s->car_sock, wfds)) {
        int err = 0;
        socklen_t elen = sizeof(err);
        if (getsockopt(s->car_sock, SOL_SOCKET, SO_ERROR, &err, &elen) < 0) {
            ESP_LOGW(TAG, "upstream connect: getsockopt failed (slot %d): errno %d", i, errno);
            close_slot(r, i, "connect failed");
        } else if (err != 0) {
            ESP_LOGW(TAG, "upstream connect failed (slot %d): errno %d", i, err);
            close_slot(r, i, "connect failed");
        } else {
            s->state = SLOT_ACTIVE;
        }
    }
}

static void relay_task(void *arg)
{
    (void)arg;
    relay_state_t r = { .listen_sock = -1, .gateway_be = 0, .host_be = 0 };
    for (int i = 0; i < RELAY_POOL_SIZE; i++) {
        r.slots[i] = (tcp_slot_t){ .state = SLOT_FREE, .phone_sock = -1, .car_sock = -1,
                                    .connect_started_ms = 0 };
    }

    /* Parsed first, before the gateway is even read: handle_accept compares against it to
     * refuse a network that advertises the dongle itself as its gateway, and that comparison
     * has to be in place before the first connection can be accepted. usb_net_start()
     * (app_main, before this task can ever reach here) already gave the USB netif this exact
     * static address, so the only way this fails is a real configuration error. */
    esp_ip4_addr_t host_ip;
    if (esp_netif_str_to_ip4(DONGLE_HOST, &host_ip) != ESP_OK) {
        ESP_LOGE(TAG, "DONGLE_HOST does not parse as an address");
        vTaskDelete(NULL);
        return;
    }
    r.host_be = host_ip.addr;

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

    /* THIS is what keeps the car's side of the dongle from ever reaching this listener — not
     * the bind below. See usb_net.h: bind() sets the pcb's address, SO_BINDTODEVICE sets its
     * interface, and only the second one is an interface filter on a weak-host stack. A
     * listener's netif_idx is inherited by every connection it accepts (lwIP tcp_in.c:711),
     * so this one call covers the whole pool. Fail closed: a listener that could not be
     * pinned is one a station on the car's network can fill, and four connections is the
     * entire pool. */
    if (usb_net_bind_socket(r.listen_sock) != ESP_OK) {
        ESP_LOGE(TAG, "cannot pin the REST relay to the USB wire — not serving it");
        close(r.listen_sock);
        vTaskDelete(NULL);
        return;
    }

    /* Bound to DONGLE_HOST rather than INADDR_ANY as well. That is not the isolation (the
     * pin above is); it is what makes this listener answer on the address the phone dialled. */
    struct sockaddr_in listen_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(DONGLE_RELAY_HTTP_PORT),
        .sin_addr.s_addr = r.host_be,
    };
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
                tcp_pending_t *p2c = &s_p2c_pending[i];
                tcp_pending_t *c2p = &s_c2p_pending[i];
                /* Backpressure: offer a side for reading only while ITS OWN outbound
                 * backlog is empty — offering it while backed up would let this task keep
                 * accepting bytes it already knows it cannot deliver. Offer the other side
                 * for writing only while there is a backlog aimed at it. A socket can land
                 * in both sets in the same pass (e.g. phone_sock readable for phone->car
                 * while also in writefds to drain a car->phone backlog) — the two
                 * directions of one TCP connection are independent. */
                if (tcp_pending_empty(p2c)) {
                    FD_SET(s->phone_sock, &rfds);
                    if (s->phone_sock > maxfd) maxfd = s->phone_sock;
                } else {
                    FD_SET(s->car_sock, &wfds);
                    if (s->car_sock > maxfd) maxfd = s->car_sock;
                }
                if (tcp_pending_empty(c2p)) {
                    FD_SET(s->car_sock, &rfds);
                    if (s->car_sock > maxfd) maxfd = s->car_sock;
                } else {
                    FD_SET(s->phone_sock, &wfds);
                    if (s->phone_sock > maxfd) maxfd = s->phone_sock;
                }
            } else if (s->state == SLOT_CONNECTING) {
                /* connect() completion is observed as writability, not readability — see
                 * handle_accept's comment. The phone side is watched too, so a hangup mid-
                 * connect is noticed rather than pinning the slot until the connect deadline
                 * — see handle_connecting. */
                FD_SET(s->phone_sock, &rfds);
                if (s->phone_sock > maxfd) maxfd = s->phone_sock;
                FD_SET(s->car_sock, &wfds);
                if (s->car_sock > maxfd) maxfd = s->car_sock;
            }
        }

        struct timeval tv = { .tv_sec = RELAY_LOOP_MS / 1000, .tv_usec = 0 };
        int nready = select(maxfd + 1, &rfds, &wfds, NULL, &tv);
        if (nready < 0 && errno != EINTR) {
            ESP_LOGW(TAG, "select: errno %d", errno);
        }

        if (nready > 0 && FD_ISSET(r.listen_sock, &rfds)) {
            handle_accept(&r);
        }

        for (int i = 0; i < RELAY_POOL_SIZE; i++) {
            tcp_slot_t *s = &r.slots[i];

            if (s->state == SLOT_CONNECTING) {
                /* Runs every pass, not only when select() found something — see
                 * handle_connecting's own comment on why the deadline needs that. */
                handle_connecting(&r, i, nready > 0, &rfds, &wfds);
                continue;
            }

            if (s->state != SLOT_ACTIVE || nready <= 0) {
                continue;
            }

            tcp_pending_t *p2c = &s_p2c_pending[i];
            tcp_pending_t *c2p = &s_c2p_pending[i];

            /* Flush before reading more: draining a backlog is what earns a throttled side
             * its way back into next pass's readfds (see the fd-set build above), so do it
             * first. The first flush below relies on the `state != SLOT_ACTIVE` check just
             * above — state is guaranteed ACTIVE here, nothing has run yet to change it.
             * Each of the three steps after it re-checks state == SLOT_ACTIVE explicitly,
             * because any earlier one in this sequence (a forwarding failure, a peer hanging
             * up) can close this same slot, and once it does its sockets are -1 — FD_ISSET
             * on a closed slot's stale fd number is exactly what must not happen. */
            if (!tcp_pending_empty(p2c) && FD_ISSET(s->car_sock, &wfds)) {
                flush_pending(&r, i, s->car_sock, p2c, "phone->car");
            }
            if (s->state == SLOT_ACTIVE && !tcp_pending_empty(c2p) &&
                FD_ISSET(s->phone_sock, &wfds)) {
                flush_pending(&r, i, s->phone_sock, c2p, "car->phone");
            }
            /* This is not the same fd-reuse hazard relay_udp.c has to survive: handle_accept
             * — the only place this file ever creates a socket — always runs once, earlier
             * in this same pass (just above), before any close_slot call here can free one.
             * So no socket this pass's select() marked ready is ever handed to a new
             * connection before this pass finishes examining it; a freed fd is only reused
             * starting the NEXT pass's handle_accept, by which time a fresh select() has
             * already run and correctly reflects it. Every socket here stays non-blocking
             * regardless (see set_nonblocking's comment) — that is defense in depth here,
             * not the reason it is needed. */
            if (s->state == SLOT_ACTIVE && FD_ISSET(s->phone_sock, &rfds)) {
                pump_read(&r, i, s->phone_sock, s->car_sock, s_phone_buf, p2c, "phone->car");
            }
            if (s->state == SLOT_ACTIVE && FD_ISSET(s->car_sock, &rfds)) {
                pump_read(&r, i, s->car_sock, s->phone_sock, s_car_buf, c2p, "car->phone");
            }
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
