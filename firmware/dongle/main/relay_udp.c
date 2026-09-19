#include "relay_udp.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include "dongle_contract.inc"
#include "dongle_clock.h"
#include "rate_gate.h"
#include "relay_stats.h"
#include "udp_sess.h"
#include "uplink.h"
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


/* The gate's job is the AVERAGE toward the phone, not the burst: a keyframe (60-100 KB)
 * must fit inside one window whole, or the gate would refuse half of every one of them at
 * the top bitrate. The burst itself is absorbed by the widened lwIP/NCM buffers, not here. */
#define VIDEO_GATE_WINDOW_MS 1000

typedef struct {
    const relay_udp_cfg_t *cfg;
    udp_sess_table_t sess;
    /* car_sock[i] mirrors sess.s[i].used exactly: valid (>=0) whenever, and only when, slot i
     * holds a live session. Kept exact even when opening a brand-new session's car-facing
     * socket fails — see handle_phone_datagram, which rolls the touch back rather than leave
     * a row marked used with no socket behind it. */
    int car_sock[UDP_SESS_MAX];
    int phone_sock;
    uint32_t gateway_be;   /* network byte order, meaningful once the wait loop below returns */
    uint32_t host_be;      /* DONGLE_HOST, parsed once; network byte order */
    rate_gate_t gate;      /* the video instance's admission toward the phone; unused otherwise */
    /* Heap, not file scope: two instances of this task now run at once, and file-scope
     * buffers shared between them would be the same race the old single-instance comment
     * here warned this design avoided. One allocation per task, sized once at relay_task's
     * start (heap_caps_malloc, MALLOC_CAP_DEFAULT) and never touched by more than the task
     * that owns it, so no lock is needed. */
    char *phone_buf;
    char *car_buf;
} relay_state_t;

#if CONFIG_DONGLE_VIDEO_BENCH
/* The bench generator (Kconfig: DONGLE_VIDEO_BENCH). One frame every 1/15 s, sized to the rate
 * the phone's last `view` asked for, cut into 1400-byte chunks behind the car's own 12-byte
 * wire header (contract/car-api.json `video.header`): proto 1, flags bit0 = key (every 45th
 * frame, the car's GOP), stream 0xFE so a receiver can tell it from the car, big-endian frame
 * and captured_ms. Paced one chunk per pass of the relay loop, whose select() timeout drops to
 * DONGLE_VIDEO_BENCH_PACE_US while a bench is on. `kbps` 0 is off — the relay behaves as built. */
#define BENCH_CHUNK_BYTES 1400
#define BENCH_HDR_BYTES   12
#define BENCH_FPS         15
#define BENCH_GOP         45
static struct {
    uint32_t kbps;
    uint16_t frame;
    uint8_t  chunk, count;
    uint32_t next_frame_ms;
    bool     in_frame;
    uint8_t  buf[BENCH_HDR_BYTES + BENCH_CHUNK_BYTES];
} s_bench;

static bool bench_view(const char *buf, int n)
{
    /* The view is JSON the dongle otherwise never parses; the one key it looks for here is
     * bench-only and never sent by the app. */
    char tmp[RELAY_BUF_LEN];
    int len = n < (int)sizeof(tmp) - 1 ? n : (int)sizeof(tmp) - 1;
    memcpy(tmp, buf, (size_t)len); tmp[len] = 0;
    const char *k = strstr(tmp, "\"bench\":");
    if (!k) return false;
    uint32_t kbps = (uint32_t)atoi(k + 8);
    if (kbps != s_bench.kbps) {
        ESP_LOGW(TAG, "bench: %u kbps (%u us per chunk)", (unsigned)kbps, (unsigned)CONFIG_DONGLE_VIDEO_BENCH_PACE_US);
        s_bench.kbps = kbps; s_bench.in_frame = false; s_bench.next_frame_ms = boot_ms();
    }
    return true;
}

static int64_t s_bench_next_us;

static bool bench_one_chunk(relay_state_t *r)
{
    uint32_t now = boot_ms();
    if (!s_bench.in_frame) {
        if ((int32_t)(now - s_bench.next_frame_ms) < 0) return false;
        s_bench.next_frame_ms += 1000 / BENCH_FPS;
        if ((int32_t)(now - s_bench.next_frame_ms) > 200) s_bench.next_frame_ms = now;   /* fell far behind: resync */
        uint32_t frame_bytes = s_bench.kbps * 1000u / 8u / BENCH_FPS;
        uint32_t count = (frame_bytes + BENCH_CHUNK_BYTES - 1) / BENCH_CHUNK_BYTES;
        s_bench.count = (uint8_t)(count < 1 ? 1 : count > 255 ? 255 : count);
        s_bench.chunk = 0; s_bench.in_frame = true; s_bench.frame++;
    }
    uint8_t *h = s_bench.buf;
    bool key = (s_bench.frame % BENCH_GOP) == 1;
    h[0] = 1; h[1] = key ? 1 : 0; h[2] = 0xFE; h[3] = 0;
    h[4] = (uint8_t)(s_bench.frame >> 8); h[5] = (uint8_t)s_bench.frame;
    h[6] = s_bench.chunk; h[7] = s_bench.count;
    h[8] = (uint8_t)(now >> 24); h[9] = (uint8_t)(now >> 16); h[10] = (uint8_t)(now >> 8); h[11] = (uint8_t)now;
    /* payload: the chunk index repeated, so a receiver could even verify content */
    memset(h + BENCH_HDR_BYTES, s_bench.chunk, BENCH_CHUNK_BYTES);
    size_t len = BENCH_HDR_BYTES + BENCH_CHUNK_BYTES;
    for (int i = 0; i < UDP_SESS_MAX; i++) {
        const udp_sess_t *s = &r->sess.s[i];
        if (!s->used) continue;
        struct sockaddr_in to = { .sin_family = AF_INET, .sin_addr.s_addr = s->addr, .sin_port = htons(s->port) };
        if (sendto(r->phone_sock, h, len, 0, (struct sockaddr *)&to, sizeof(to)) < 0) {
            relay_stats_video_dropped(relay_stats_shared());
        } else {
            relay_stats_video_forwarded(relay_stats_shared(), (uint32_t)len);
        }
    }
    if (++s_bench.chunk >= s_bench.count) s_bench.in_frame = false;
    return true;
}

/* Every chunk that has come due since the last pass, at DONGLE_VIDEO_BENCH_PACE_US apart on
 * the clock — not one per pass: select()'s timeout rounds up to a FreeRTOS tick (10 ms), so a
 * pass runs ~50 times a second and one chunk per pass capped the bench at ~570 kbit/s whatever
 * it was asked for. Bounded per pass so a stall cannot turn into an unbounded burst. */
static void bench_tick(relay_state_t *r)
{
    if (s_bench.kbps == 0) return;
    int64_t now_us = esp_timer_get_time();
    if (s_bench_next_us == 0 || now_us - s_bench_next_us > 200000) s_bench_next_us = now_us;
    int budget = 64;
    while (now_us >= s_bench_next_us && budget-- > 0) {
        if (!bench_one_chunk(r)) { s_bench_next_us = now_us + 1000; break; }   /* between frames */
        /* PACE_US 0 spreads a frame's chunks evenly over the frame period — a steady offered
         * load equal to the requested rate, which is what a drain ceiling is measured
         * against; a fixed pace measures burst tolerance instead (the car's own shape). */
        s_bench_next_us += CONFIG_DONGLE_VIDEO_BENCH_PACE_US ? CONFIG_DONGLE_VIDEO_BENCH_PACE_US
                                                            : (1000000 / BENCH_FPS) / (s_bench.count ? s_bench.count : 1);
    }
}
#endif /* CONFIG_DONGLE_VIDEO_BENCH */

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

/* One ephemeral UDP socket, connect()ed to gateway:cfg->port. connect() on a
 * datagram socket sets a default destination for send() and, just as importantly, filters
 * what recv() will hand back — only datagrams from that exact address:port arrive on this
 * socket, so the socket a reply shows up on identifies the session with no lookup, per the
 * brief's "one socket per session". */
static int open_car_sock(const relay_state_t *r)
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
    if (r->gateway_be == r->host_be) {
        static log_throttle_t s_throttle = LOG_THROTTLE_INIT;
        if (log_throttle_ok(&s_throttle, boot_ms())) {
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
        .sin_addr.s_addr = r->gateway_be,
        .sin_port = htons(r->cfg->port),
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

#if CONFIG_DONGLE_VIDEO_BENCH
    if (r->cfg->video && bench_view(buf, n)) {
        /* The bench answers this peer itself: a session with no car-facing socket, so it
         * works with the car switched off and no gateway learned. close_car_sock tolerates
         * the -1 when the slot is later reused or expired. */
        udp_sess_touch(&r->sess, addr, port, boot_ms());
        return;
    }
#endif
    bool is_new = udp_sess_find(&r->sess, addr, port) < 0;

    /* The socket comes FIRST, before the table is touched, and the touch is skipped entirely
     * if it cannot be had. udp_sess_touch on a full table evicts: it overwrites the victim's
     * (addr, port) and hands back its slot. Opening afterwards meant a failure — the socket
     * table exhausted, connect() refusing, or open_car_sock's own gateway == host check —
     * destroyed a live session on behalf of a peer that could not be served anyway, and the
     * rollback could not undo it: setting `used = false` frees the slot but the evicted
     * phone's address is already gone, along with its socket. A phone mid-drive lost its relay
     * to a stray datagram from an unrelated source port. This way round the failure costs one
     * dropped datagram and nothing else. */
    int fresh = -1;
    if (is_new) {
        fresh = open_car_sock(r);
        if (fresh < 0) {
            ESP_LOGW(TAG, "dropping a datagram: no car-facing socket for a new peer");
            return;
        }
    }

    int idx = udp_sess_touch(&r->sess, addr, port, boot_ms());

    if (is_new) {
        /* A slot won by eviction can still carry the socket of the peer it displaced. */
        close_car_sock(r, idx);
        r->car_sock[idx] = fresh;
    }

    /* Scored either way (uplink.h): a send that went out and a send that failed are the same
     * silence toward an uplink that never answers, and the streak is measured in time from
     * its first send, so a 10 Hz drive stream and the REST relay's polls read the same clock.
     * The station's own view is the guard: a streak while it says `connected` is the
     * association the car's softAP forgot, and a re-join is the only exit. While it says
     * anything else the silence has a reason wifi_state already owns — its budget and its
     * hold — and kicking from here would be the radio hunting an absent car forever, which
     * the design forbids. */
    bool kick;
    if (send(r->car_sock[idx], buf, (size_t)n, 0) < 0) {
        int err = errno;
        relay_stats_failed(relay_stats_shared(), err, boot_ms());
        kick = uplink_failed(uplink_shared(), boot_ms());
        errno = err;
        /* Rate-limited: a Wi-Fi drop fails every send, and udp_sess_touch (above) refreshes
         * this session's deadline on every phone datagram regardless of whether the send that
         * follows succeeds — so the session cannot age out while the phone keeps streaming,
         * and an unthrottled log here would be one ESP_LOGW per datagram, at 10 Hz, on the
         * dongle's sole UART console, for the whole outage. Same idiom as rt_link.c's
         * last_log. */
        static log_throttle_t s_throttle = LOG_THROTTLE_INIT;
        if (log_throttle_ok(&s_throttle, boot_ms())) {
            ESP_LOGW(TAG, "%s: phone->car send failed on slot %d: errno %d", r->cfg->name,
                     idx, errno);
        }
    } else {
        kick = uplink_sent(uplink_shared(), boot_ms());
        /* The video instance's phone->car direction is view datagrams, not control frames —
         * counting them here would colour to_car_hz with a channel relay_stats.h's own
         * comment says belongs to the control loop alone. */
        if (!r->cfg->video) relay_stats_forwarded(relay_stats_shared(), true);
    }
    if (kick) {
        if (wifi_sta_connected()) {
            ESP_LOGW(TAG, "%s: the car has answered nothing for %u ms of sends while the "
                          "station says connected — rejoining", r->cfg->name,
                     (unsigned)UPLINK_DEAD_AFTER_MS);
            esp_err_t jerr = wifi_sta_rejoin();
            if (jerr != ESP_OK) ESP_LOGW(TAG, "rejoin refused: %s", esp_err_to_name(jerr));
        }
    }
}

/* Car -> phone. sendto, not send: the phone-facing socket is the one shared listener, bound
 * to DONGLE_HOST:cfg->port, so every reply must name which phone it is for — the
 * address and port udp_sess recorded for this slot when the session was created. */
static void handle_car_datagram(relay_state_t *r, int idx, const char *buf, int n)
{
    uplink_heard(uplink_shared());   /* the car spoke: however long the silence was, it is over */
    /* The video instance only: admitted against rate_gate before it is ever handed to
     * lwIP/TinyUSB, so a refusal is counted here rather than lost silently in the NTB pool
     * (rate_gate.h). The control instance is never throttled — cfg->video is false for it,
     * so this is skipped entirely and its behaviour is unchanged. */
    if (r->cfg->video && !rate_gate_admit(&r->gate, boot_ms(), (uint32_t)n)) {
        relay_stats_video_dropped(relay_stats_shared());
        return;
    }

    const udp_sess_t *s = &r->sess.s[idx];
    struct sockaddr_in to = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = s->addr,
        .sin_port = htons(s->port),
    };
    if (sendto(r->phone_sock, buf, (size_t)n, 0, (struct sockaddr *)&to, sizeof(to)) < 0) {
        /* A chunk the gate admitted and the wire still refused — the NTB pool was full
         * after all — is a video drop like any other: it goes into relay.video_dropped,
         * the figure stage 3 measures the ceiling against, not into the errno pair. That
         * pair is the control channel's fault record: a video burst that fails ten sends
         * would otherwise stamp last_error with a fault the control loop never had. */
        if (r->cfg->video) {
            relay_stats_video_dropped(relay_stats_shared());
        } else {
            relay_stats_failed(relay_stats_shared(), errno, boot_ms());
        }
        /* Rate-limited for the same reason as the phone->car send above. */
        static log_throttle_t s_throttle = LOG_THROTTLE_INIT;
        if (log_throttle_ok(&s_throttle, boot_ms())) {
            ESP_LOGW(TAG, "%s: car->phone send failed on slot %d: errno %d", r->cfg->name,
                     idx, errno);
        }
    } else if (r->cfg->video) {
        relay_stats_video_forwarded(relay_stats_shared(), (uint32_t)n);
    } else {
        relay_stats_forwarded(relay_stats_shared(), false);
    }
}

static void expire_sessions(relay_state_t *r)
{
    uint32_t freed = udp_sess_expire(&r->sess, boot_ms(), UDP_SESS_IDLE_MS);
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
    relay_state_t r = { .cfg = (const relay_udp_cfg_t *)arg, .phone_sock = -1,
                         .gateway_be = 0, .host_be = 0 };
    udp_sess_init(&r.sess);
    for (int i = 0; i < UDP_SESS_MAX; i++) r.car_sock[i] = -1;

    /* Heap, not the old file-scope statics: two instances of this task run at once now, and
     * a shared buffer between them would race. MALLOC_CAP_DEFAULT: PSRAM is fine for this —
     * neither buffer crosses a DMA boundary, it is memcpy'd out by recvfrom/sendto like any
     * other userspace buffer. */
    r.phone_buf = heap_caps_malloc(RELAY_BUF_LEN, MALLOC_CAP_DEFAULT);
    r.car_buf = heap_caps_malloc(RELAY_BUF_LEN, MALLOC_CAP_DEFAULT);
    if (r.phone_buf == NULL || r.car_buf == NULL) {
        ESP_LOGE(TAG, "%s: cannot allocate its %d-byte buffers", r.cfg->name, RELAY_BUF_LEN);
        vTaskDelete(NULL);
        return;
    }
    /* The control instance's gate is never consulted (handle_car_datagram checks cfg->video
     * first), but initialising it unconditionally means relay_state_t never carries a field
     * that is sometimes garbage. See VIDEO_GATE_WINDOW_MS for why the window is 1000 ms. */
    rate_gate_init(&r.gate, DONGLE_RELAY_VIDEO_MAX_KBPS, VIDEO_GATE_WINDOW_MS);

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
     * serve both this relay and relay_tcp's — so this is an ordinary retry loop, not a wait on
     * anything. */
#if CONFIG_DONGLE_VIDEO_BENCH
    /* The bench serves the phone with no car and no gateway; the loop below still re-aims
     * the moment a gateway appears, exactly as it does after any later re-join. */
    if (!r.cfg->video)
#endif
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
        ESP_LOGE(TAG, "cannot pin %s to the USB wire — not serving it", r.cfg->name);
        close(r.phone_sock);
        vTaskDelete(NULL);
        return;
    }

    /* Bound to DONGLE_HOST rather than INADDR_ANY as well. That is not the isolation (the
     * pin above is); it is what makes every reply leave with the address the phone sent to,
     * so a phone's socket accepts it. */
    struct sockaddr_in phone_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(r.cfg->port),
        .sin_addr.s_addr = r.host_be,
    };
    if (bind(r.phone_sock, (struct sockaddr *)&phone_addr, sizeof(phone_addr)) < 0) {
        ESP_LOGE(TAG, "%s: phone-facing bind %s:%u: errno %d", r.cfg->name, DONGLE_HOST,
                 r.cfg->port, errno);
        close(r.phone_sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "%s up on %s:%u", r.cfg->name, DONGLE_HOST, r.cfg->port);

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
#if CONFIG_DONGLE_VIDEO_BENCH
        /* Never a zero timeout: that is a spin at priority 6 that starves TinyUSB and trips the idle watchdog. */
        if (r.cfg->video && s_bench.kbps != 0) tv = (struct timeval){ .tv_sec = 0, .tv_usec = CONFIG_DONGLE_VIDEO_BENCH_PACE_US ? CONFIG_DONGLE_VIDEO_BENCH_PACE_US : 1000 };
#endif
        int nready = select(maxfd + 1, &rfds, NULL, NULL, &tv);
#if CONFIG_DONGLE_VIDEO_BENCH
        if (r.cfg->video) bench_tick(&r);
#endif
        if (nready > 0) {
            if (FD_ISSET(r.phone_sock, &rfds)) {
                struct sockaddr_in from;
                socklen_t flen = sizeof(from);
                /* Non-blocking (set_nonblocking, above): a stale readiness bit — e.g. this
                 * fd was reused by an eviction after select() sampled it but before this line
                 * runs — costs an EAGAIN here, not a wait. */
                int n = recvfrom(r.phone_sock, r.phone_buf, RELAY_BUF_LEN, 0,
                                  (struct sockaddr *)&from, &flen);
                if (n == RELAY_BUF_LEN) {
                    ESP_LOGW(TAG, "%s: phone->car datagram over %d bytes, dropped whole",
                              r.cfg->name, RELAY_DATAGRAM_MAX);
                } else if (n > 0 && from.sin_family == AF_INET) {
                    handle_phone_datagram(&r, r.phone_buf, n, &from);
                } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                    ESP_LOGW(TAG, "%s: phone-facing recvfrom: errno %d", r.cfg->name, errno);
                }
            }
            for (int i = 0; i < UDP_SESS_MAX; i++) {
                if (r.car_sock[i] < 0 || !FD_ISSET(r.car_sock[i], &rfds)) continue;
                /* Same non-blocking guarantee as the phone-facing read above. */
                int n = recv(r.car_sock[i], r.car_buf, RELAY_BUF_LEN, 0);
                if (n == RELAY_BUF_LEN) {
                    ESP_LOGW(TAG, "%s: car->phone datagram over %d bytes, dropped whole (slot %d)",
                              r.cfg->name, RELAY_DATAGRAM_MAX, i);
                } else if (n > 0) {
                    handle_car_datagram(&r, i, r.car_buf, n);
                } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                    ESP_LOGW(TAG, "%s: car-facing recv (slot %d): errno %d", r.cfg->name, i, errno);
                }
            }
        } else if (nready < 0 && errno != EINTR) {
            /* select() failing returns immediately, so this pass has no wait left in it. An
             * error that persists — EBADF the instant any fd in the sets is dead, which is
             * what a socket closed underneath this task produces — would otherwise make this
             * task spin at full speed with an unthrottled log per iteration, which is worse
             * for the device than the fault being reported. Take the pass's wait
             * here instead, and rate-limit the line to the same 1 Hz as this file's other
             * repeating warnings. expire_sessions below still runs every pass. */
            static log_throttle_t s_throttle = LOG_THROTTLE_INIT;
            if (log_throttle_ok(&s_throttle, boot_ms())) {
                ESP_LOGW(TAG, "%s: select: errno %d", r.cfg->name, errno);
            }
            vTaskDelay(pdMS_TO_TICKS(RELAY_LOOP_MS));
        }

        expire_sessions(&r);

        /* Session occupancy, published at the END of the pass. Counted here rather than kept
         * as a running counter for the reason relay_tcp.c gives for its own slots: every place
         * that opens or closes one would otherwise have to remember to adjust it, and a
         * miscount there would be silent. car_sock[i] >= 0 mirrors sess.s[i].used exactly
         * (relay_state_t's own comment), so this is the live count without a second structure
         * to keep in step.
         *
         * At the end and not while building the fd set above, which is where it used to be:
         * that placement counted sessions before select()'s wait AND before expire_sessions()
         * ran just now, so a session that aged out during this pass stayed in the published
         * figure until the next pass closed the window — a whole RELAY_LOOP_MS later, longer
         * if select() took its full timeout. The display reads this five times a second off a
         * number that can only move once a second; the least it can be is the count as of the
         * end of the pass that produced it. */
        int live = 0;
        for (int i = 0; i < UDP_SESS_MAX; i++) {
            if (r.car_sock[i] >= 0) live++;
        }
        if (r.cfg->video) {
            relay_stats_video_slots(relay_stats_shared(), (uint8_t)live);
        } else {
            relay_stats_udp_slots(relay_stats_shared(), (uint8_t)live);
        }
    }
}

esp_err_t relay_udp_start(const relay_udp_cfg_t *cfg)
{
    /* relay_stats_init(relay_stats_shared()) moved to main.c: it must run once, before EITHER
     * instance's task exists, and calling it again here on the second call would zero counters
     * the first instance had already started publishing. See main.c's own comment. */
    if (xTaskCreate(relay_task, cfg->name, 4096, (void *)cfg, cfg->priority, NULL) != pdPASS) {
        return ESP_FAIL;
    }
    return ESP_OK;
}
