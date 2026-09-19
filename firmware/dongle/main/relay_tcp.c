#include "relay_tcp.h"

#include <errno.h>
#include <fcntl.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include "dongle_contract.inc"
#include "dongle_clock.h"
#include "relay_stats.h"
#include "uplink.h"
#include "tcp_pending.h"
#include "usb_net.h"
#include "wifi_sta.h"

static const char *TAG = "relay_tcp";

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

/* How long a SLOT_ACTIVE slot may go without a single byte moving in either direction before
 * this relay gives up on it. Without this the state has no clock at all: every other exit
 * from SLOT_ACTIVE is event-driven, and a USB detach produces no event whatsoever — usb_net.c
 * calls the netif start action once and never a stop, and the TinyUSB component reports no
 * link state, so lwIP is never told the wire went away. There is no FIN, no RST, and select()
 * never marks the socket readable or errored; the slot stays ACTIVE forever. Four of those
 * and the car's whole REST surface through the dongle is dead until a power cycle, with
 * /status still reporting a healthy dongle.
 *
 * Sixty seconds, sized against what a legitimate quiet connection needs rather than against
 * what is convenient to test. The longest legitimate silence on an open REST connection is a
 * kept-alive one between two requests while a person decides something — the calibration
 * wizard's spin-a-wheel-and-answer step is the concrete case, and that is human-scale
 * seconds, not minutes. A firmware upload is never silent for long: the car's OTA path erases
 * its partition inside esp_ota_begin, several seconds, not a minute. So a minute clears every
 * legitimate pause with room to spare, while turning "dead until a power cycle" into "heals
 * itself a minute after the phone goes away". Deliberately much longer than relay_udp's
 * UDP_SESS_IDLE_MS (10 s): a control stream that is quiet for ten seconds has ended, whereas
 * a REST connection quiet for ten seconds is just a user reading the screen. */
#define RELAY_ACTIVE_IDLE_MS 60000u

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
/* Pool size is a design number, not a detail — the spec says so. Four: the app can have a
 * config POST and a firmware upload in flight at once, and a pool of one would deadlock the
 * second behind the first. Four leaves room for the browser-style parallelism a REST client
 * may use without letting a leaked slot starve the pool. */
#define RELAY_POOL_SIZE 4

/* Back in this file rather than the header, where it lived only so relay_udp.c could pass it
 * into relay_stats_init's udp_max/tcp_max — parameters nothing read. Nothing outside this
 * translation unit uses the pool's size. */
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
    /* When this slot last moved a byte, in either direction. Set when the slot is created so
     * it is never stale, refreshed on every successful read and every successful write, and
     * read only while state == SLOT_ACTIVE — the same idiom relay_udp uses for its sessions,
     * where udp_sess_touch refreshes a deadline udp_sess_expire enforces. */
    uint32_t last_active_ms;
} tcp_slot_t;

typedef struct {
    tcp_slot_t slots[RELAY_POOL_SIZE];
    int listen_sock;
    uint32_t gateway_be;   /* network byte order, meaningful once the wait loop below returns */
    uint32_t host_be;      /* DONGLE_HOST, parsed once; network byte order */
} relay_state_t;

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

/* A connection attempt toward the car that is now known to have gone unanswered — the SYN
 * could not leave, the phone gave up waiting on it, or this relay's own deadline did. Scored
 * with the UDP relay's sends (uplink.h), because during a launch gate or an update's reboot
 * watch these polls may be the only traffic toward the car, and the association the car
 * forgot has to be noticed from them too. Same guard, same reason as relay_udp.c: a streak
 * while the station says `connected` is that forgotten association, and a re-join is the only
 * exit; while it says anything else the silence has a reason wifi_state already owns. */
static void unanswered_connect(void)
{
    if (uplink_failed(uplink_shared(), boot_ms()) && wifi_sta_connected()) {
        ESP_LOGW(TAG, "tcp: the car has answered nothing for %u ms of sends while the station "
                      "says connected — rejoining", (unsigned)UPLINK_DEAD_AFTER_MS);
        esp_err_t jerr = wifi_sta_rejoin();
        if (jerr != ESP_OK) ESP_LOGW(TAG, "rejoin refused: %s", esp_err_to_name(jerr));
    }
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
        r->slots[idx].last_active_ms = boot_ms();   /* bytes moved: the slot is not idle */
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
    relay_stats_failed(relay_stats_shared(), errno, boot_ms());
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
        /* A successful read is progress on its own, whatever the forward attempt below does:
         * a slot whose phone is talking must not age out because the car is momentarily
         * refusing more bytes. */
        r->slots[idx].last_active_ms = boot_ms();
        if (src == r->slots[idx].car_sock) uplink_heard(uplink_shared());
        int w = send(dst, scratch, (size_t)n, 0);
        if (w == n) {
            return;   /* the common case: forwarded whole, nothing left pending */
        }
        if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            relay_stats_failed(relay_stats_shared(), errno, boot_ms());
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
            static log_throttle_t s_throttle = LOG_THROTTLE_INIT;
            if (log_throttle_ok(&s_throttle, boot_ms())) {
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
        static log_throttle_t s_throttle = LOG_THROTTLE_INIT;
        if (log_throttle_ok(&s_throttle, boot_ms())) {
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
        static log_throttle_t s_throttle = LOG_THROTTLE_INIT;
        if (log_throttle_ok(&s_throttle, boot_ms())) {
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
        static log_throttle_t s_throttle = LOG_THROTTLE_INIT;
        if (log_throttle_ok(&s_throttle, boot_ms())) {
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
        uplink_heard(uplink_shared());   /* the SYN was answered */
    } else if (errno == EINPROGRESS) {
        r->slots[idx].state = SLOT_CONNECTING;
        r->slots[idx].connect_started_ms = boot_ms();
    } else {
        /* The SYN could not even leave — unanswered from the start (unanswered_connect). */
        int cerr = errno;
        unanswered_connect();
        errno = cerr;
        /* Rate-limited: a car actively refusing the port (ECONNREFUSED comes back fast, no
         * SYN retries involved) fails this on every attempt a retrying client makes. */
        static log_throttle_t s_throttle = LOG_THROTTLE_INIT;
        if (log_throttle_ok(&s_throttle, boot_ms())) {
            ESP_LOGW(TAG, "upstream connect: errno %d", errno);
        }
        close(car);
        close(c);
        return;
    }
    r->slots[idx].phone_sock = c;
    r->slots[idx].car_sock = car;
    /* Seeded here rather than only on the SLOT_ACTIVE transition, so the idle deadline is
     * never read against an uninitialised (or a previous occupant's) timestamp — including on
     * the cr == 0 path just above, which goes straight to SLOT_ACTIVE. */
    r->slots[idx].last_active_ms = boot_ms();
}

/* One SLOT_CONNECTING slot, checked every pass regardless of what select() returned. Three
 * things can end a connect attempt, and Step 6 of the brief ("either half returning 0 or an
 * error closes both and frees the slot") is not scoped to SLOT_ACTIVE — it applies here too:
 *
 * 1. The deadline. Checked first and unconditionally, because a car that never answers at
 *    all — the ordinary failure mode of "the car is off", not an exotic one — leaves
 *    select() timing out with nothing ready, pass after pass; only a check that runs whether
 *    or not select() found anything can ever catch that.
 * 2. The phone hanging up while this relay is still waiting on the car. A phone that gave
 *    up on the car is a connection attempt the car did not answer in time, and the moment it
 *    gives up is when that is known — so it is scored (unanswered_connect) right then, not
 *    when this relay's own, longer deadline fires. That is what puts the launch ladder's and
 *    the update watch's /version polls on the same clock as the UDP relay's datagrams: the
 *    phone abandons each one after its own timeout, and the streak grows at that pace rather
 *    than at RELAY_CONNECT_TIMEOUT_MS per poll (AJM-124).
 *
 *    To see the hangup at all, the request the phone sent right after connect() (an ordinary
 *    thing to do, without waiting for anything from the far end) has to be READ, not peeked:
 *    lwIP queues the FIN behind the data, so a recv() sees EOF only once the bytes before it
 *    are consumed, and a peek that leaves them in place also leaves select() level-triggered
 *    on them — a priority-5 task returning at once on every pass, for the whole connect. The
 *    bytes are appended to this slot's phone->car backlog, where they wait for the car exactly
 *    as a chunk pump_read could not forward would, and flush_pending sends them the first pass
 *    the slot is ACTIVE. The backlog's size bounds this: a request longer than one backlog (a
 *    firmware upload) fills it, the phone side then leaves readfds until there is room again,
 *    and a phone hanging up mid-upload is noticed when the connect resolves or its deadline
 *    fires — the case the deadline exists for, and one no poll ever produces.
 * 3. The connect() itself resolving, via the SO_ERROR/writable check the brief describes.
 *
 * Any of the three can close the slot; each returns immediately after doing so rather than
 * falling into the next check on a slot that no longer exists. */
static void handle_connecting(relay_state_t *r, int i, bool had_ready, fd_set *rfds,
                               fd_set *wfds)
{
    tcp_slot_t *s = &r->slots[i];

    if ((uint32_t)(boot_ms() - s->connect_started_ms) >= RELAY_CONNECT_TIMEOUT_MS) {
        /* Same wraparound-safe elapsed-time idiom as udp_sess_expire's: this subtraction
         * elapses correctly even across the millisecond counter's ~49.7-day wrap. */
        ESP_LOGW(TAG, "upstream connect timed out (slot %d)", i);
        close_slot(r, i, "connect timed out");
        /* A SYN that left and was never answered — by the relay's own clock, when the phone
         * held on longer than it. */
        unanswered_connect();
        return;
    }

    if (had_ready && FD_ISSET(s->phone_sock, rfds)) {
        /* Offered for reading only while the backlog has room (relay_task's fd-set build),
         * and read no further than that room, so a segment never lands half in the backlog
         * and half nowhere. The scratch is pump_read's, RELAY_BUF_LEN long — the backlog's
         * own length, so the room always fits it. */
        tcp_pending_t *p2c = &s_p2c_pending[i];
        int n = recv(s->phone_sock, s_phone_buf, (size_t)tcp_pending_room(p2c), 0);
        if (n > 0) {
            tcp_pending_append(p2c, s_phone_buf, n);
        } else if (n == 0) {
            close_slot(r, i, "phone gave up during connect");
            unanswered_connect();
            return;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            /* Mirrors pump_read's "recv failed" path: a real error, not an ordinary hangup,
             * so the errno is worth keeping — this whole connect-in-progress window is the
             * least-proven part of the file. Scored the same: the phone is gone before the
             * car answered, whichever way it went. */
            ESP_LOGW(TAG, "phone recv during connect failed (slot %d): errno %d", i, errno);
            close_slot(r, i, "phone gave up during connect");
            unanswered_connect();
            return;
        }
        /* EAGAIN: a spurious wakeup, nothing to do. Either way, keep waiting on the car. */
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
            uplink_heard(uplink_shared());   /* the SYN was answered */
            /* The idle clock starts when the slot goes ACTIVE, not when the phone connected:
             * time spent waiting for the car to answer is already bounded by
             * RELAY_CONNECT_TIMEOUT_MS, and charging it twice would cut a legitimate
             * connection's first quiet period short. */
            s->last_active_ms = boot_ms();
        }
    }
}

static void relay_task(void *arg)
{
    (void)arg;
    relay_state_t r = { .listen_sock = -1, .gateway_be = 0, .host_be = 0 };
    for (int i = 0; i < RELAY_POOL_SIZE; i++) {
        r.slots[i] = (tcp_slot_t){ .state = SLOT_FREE, .phone_sock = -1, .car_sock = -1,
                                    .connect_started_ms = 0, .last_active_ms = 0 };
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
                 * connect is noticed — and scored — the moment it happens rather than at the
                 * connect deadline (see handle_connecting). Watched while the phone->car
                 * backlog has ROOM, not only while it is empty as an ACTIVE slot's rule has
                 * it: the request handle_connecting reads goes into that backlog and cannot
                 * leave it before the car is connected, so "empty" would drop the phone from
                 * readfds the moment its request arrived and hide the hangup queued behind
                 * it until the deadline — the very delay this exists to remove. A phone that
                 * has sent its request and is silently waiting keeps select() waiting too
                 * (its bytes are consumed, nothing is level-triggered); one whose request
                 * outgrew the backlog leaves readfds until the slot is ACTIVE and
                 * flush_pending has made room. The car side is NOT offered for writing to
                 * flush that backlog here — writable means the connect resolved, and the
                 * flush waits for ACTIVE. */
                if (tcp_pending_room(&s_p2c_pending[i]) > 0) {
                    FD_SET(s->phone_sock, &rfds);
                    if (s->phone_sock > maxfd) maxfd = s->phone_sock;
                }
                FD_SET(s->car_sock, &wfds);
                if (s->car_sock > maxfd) maxfd = s->car_sock;
            }
        }
        struct timeval tv = { .tv_sec = RELAY_LOOP_MS / 1000, .tv_usec = 0 };
        int nready = select(maxfd + 1, &rfds, &wfds, NULL, &tv);
        if (nready < 0 && errno != EINTR) {
            /* select() failing returns immediately, so this pass has no wait left in it. An
             * error that persists — EBADF the instant any fd in the sets is dead, which is
             * what a socket closed underneath this task produces — would otherwise make a
             * priority-5 task spin at full speed with an unthrottled log per iteration, which
             * is worse for the device than the fault being reported. Take the pass's wait
             * here instead, and rate-limit the line to the same 1 Hz as every other repeating
             * warning in this file. The deadline checks below still run every pass. */
            static log_throttle_t s_throttle = LOG_THROTTLE_INIT;
            if (log_throttle_ok(&s_throttle, boot_ms())) {
                ESP_LOGW(TAG, "select: errno %d", errno);
            }
            vTaskDelay(pdMS_TO_TICKS(RELAY_LOOP_MS));
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

            if (s->state != SLOT_ACTIVE) {
                continue;
            }

            /* Runs every pass, not only when select() found something — for the same reason
             * handle_connecting's deadline does. The failure this exists for (a USB detach,
             * which lwIP is never told about) is precisely the one where select() reports
             * nothing, pass after pass, forever: a check gated on activity could never see
             * it. See RELAY_ACTIVE_IDLE_MS for the sizing, and note the same wraparound-safe
             * subtraction udp_sess_expire uses. */
            if ((uint32_t)(boot_ms() - s->last_active_ms) >= RELAY_ACTIVE_IDLE_MS) {
                close_slot(&r, i, "idle timeout");
                continue;
            }

            if (nready <= 0) {
                continue;
            }

            tcp_pending_t *p2c = &s_p2c_pending[i];
            tcp_pending_t *c2p = &s_c2p_pending[i];

            /* Flush before reading more: draining a backlog is what earns a throttled side
             * its way back into next pass's readfds (see the fd-set build above), so do it
             * first. The first flush below relies on the `state != SLOT_ACTIVE` check above —
             * state is guaranteed ACTIVE here, since the only thing between that check and
             * this line is the idle deadline, which `continue`s when it closes the slot.
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

        /* Slot occupancy, counted once per pass rather than kept as a running counter: every
         * close_slot() and every handle_accept() would otherwise need to remember to adjust
         * it, and a miscount there would be silent.
         *
         * At the END of the pass, where it can see this pass's own work. Counted while
         * building the fd set — where it used to be — it was taken before select()'s wait,
         * before handle_accept() took a slot and before every close_slot() above freed one, so
         * the published figure was a full RELAY_LOOP_MS behind the pool it described. */
        int busy = 0;
        for (int i = 0; i < RELAY_POOL_SIZE; i++) {
            if (r.slots[i].state != SLOT_FREE) busy++;
        }
        relay_stats_tcp_slots(relay_stats_shared(), (uint8_t)busy);
    }
}

esp_err_t relay_tcp_start(void)
{
    if (xTaskCreate(relay_task, "relay_tcp", 4096, NULL, 5, NULL) != pdPASS) {
        return ESP_FAIL;
    }
    return ESP_OK;
}
