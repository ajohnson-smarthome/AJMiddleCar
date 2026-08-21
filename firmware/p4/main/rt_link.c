#include "rt_link.h"
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "esp_log.h"
#include "esp_app_desc.h"
#include "esp_task_wdt.h"
#include "cfg_table.inc"
#include "control_proto.h"
#include "watchdog.h"
#include "recovery.h"
#include "telemetry.h"
#include "identity.h"
#include "car.h"
#include "link.h"

static const char *TAG = "rt";

/* The receive timeout, and therefore the loop's beat. Short enough that the watchdog
   deadline is measured to within a tick, long enough that an idle car is not spinning. */
#define TICK_MS 20
#define PUSH_MS (1000 / RT_TELEMETRY_HZ)

/* Everything below is touched only by the rt_link task, except the two counters, which
   telemetry reads from whichever task is gathering. */
static volatile uint32_t s_frames;
static volatile uint32_t s_trips;

/* The owner is (address, port, session id), learned from recvfrom rather than from a
   socket table — which is what "strictly one client, last connect wins" means when the
   transport has no connections. A datagram from anyone else is dropped; the only way in
   is a hello, and a hello always wins. */
static struct sockaddr_in s_owner;
static bool               s_have_owner;
static char               s_sid[CONTROL_SID_MAX];
static uint32_t           s_last_seq;
static bool               s_have_seq;

/* The control watchdog, formerly watchdog.c's software timer. `s_armed` keeps it quiet
   until traffic has actually started, so a car nobody has driven yet never "loses" a
   link it never had. */
static uint32_t s_last_feed_ms;
static bool     s_armed;

uint32_t rt_link_frames(void)    { return s_frames; }
uint32_t rt_link_wdt_trips(void) { return s_trips; }

static uint32_t now_ms(void) {
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static bool same_peer(const struct sockaddr_in *a, const struct sockaddr_in *b) {
    return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}

static void log_peer(const char *what, const struct sockaddr_in *p) {
    char ip[16];
    inet_ntoa_r(p->sin_addr, ip, sizeof(ip));
    ESP_LOGI(TAG, "%s %s:%u", what, ip, (unsigned)ntohs(p->sin_port));
}

/* The reply carries identity, so "is this our car" is answered on the first exchange
   over the channel that then carries telemetry — the app needs no separate probe. */
static void send_hello_reply(int sock, const char *sid, const struct sockaddr_in *to) {
    char buf[RT_MAX_DATAGRAM];
    int n = snprintf(buf, sizeof(buf),
                     "{\"proto\":%d,\"hello\":\"%s\",\"device\":\"" CAR_DEVICE_ID "\",\"fw\":\"%s\"}",
                     RT_PROTO, sid, esp_app_get_description()->version);
    if (n < 0 || n >= (int)sizeof(buf)) {
        /* Only reachable if the firmware version string grows absurdly. A truncated
           identity is worse than none: it would parse as a different car. */
        ESP_LOGE(TAG, "hello reply does not fit a datagram");
        return;
    }
    if (sendto(sock, buf, (size_t)n, 0, (const struct sockaddr *)to, sizeof(*to)) < 0) {
        ESP_LOGW(TAG, "hello reply failed: errno %d", errno);
    }
}

static void adopt(const struct sockaddr_in *from, const char *sid) {
    s_owner      = *from;
    s_have_owner = true;
    s_have_seq   = false;   /* a new session counts from wherever it likes */
    s_armed      = false;   /* arm on the first command, not on the handshake */
    snprintf(s_sid, sizeof(s_sid), "%s", sid);
    /* A previous session's goodbye left SAFE holding zero. Release it here rather than
       there, so the actuator is free for the console and the calibration wizard in
       between, and free for this session's first command now. */
    link_release(LINK_SRC_SAFE);
    log_peer("session adopted from", from);
}

static void on_bye(void) {
    ESP_LOGI(TAG, "goodbye — stopping, and not retreating");
    /* First, in case a retreat is already running from an earlier dropout: a frame from
       the driver is what ends one, and this frame says stop. Recording it also leaves a
       stationary newest breadcrumb, so the next retreat starts from a car at rest. */
    recovery_note_command(0.0f, 0.0f);
    car_stop(LINK_SRC_SAFE);
    link_release(LINK_SRC_SAFE);
    /* This is the whole point of carrying a goodbye on the wire: silence that was
       announced is not silence that means the driver is out of range. */
    s_armed      = false;
    s_have_owner = false;
    s_have_seq   = false;
}

static void on_command(const control_frame_t *f) {
    s_frames++;
    /* A parsed frame proves the link is alive, which is the only thing this watchdog
       measures — actuator health is a separate question, answered by bus_ok. The
       breadcrumb IS gated on the grant: a refused command never moved the car, so
       recording it would corrupt the path the retreat retraces. */
    s_last_feed_ms = now_ms();
    s_armed = true;
    if (car_drive(LINK_SRC_RT, f->t, f->y)) {
        recovery_note_command(f->t, f->y);
    }
}

static void on_datagram(int sock, const char *buf, int n, const struct sockaddr_in *from) {
    control_frame_t f;
    if (control_parse_frame(buf, (size_t)n, RT_MAX_DATAGRAM, &f) != 0) {
        /* Rate-limited: whatever is sending nonsense is usually sending it at a rate. */
        static uint32_t last_log;
        uint32_t t = now_ms();
        if ((uint32_t)(t - last_log) > 1000) {
            last_log = t;
            ESP_LOGW(TAG, "dropped a bad or oversized datagram (%d bytes)", n);
        }
        return;
    }

    if (f.has_hello) {
        if (!f.has_proto || f.proto != RT_PROTO) {
            /* Answer anyway — the reply names our version, so the app can say "this car
               speaks a protocol I do not" instead of searching forever — but do not
               adopt. A session neither side can parse is worse than no session, and the
               forced-update gate exists to make this state brief. */
            ESP_LOGW(TAG, "hello with proto %u, this car speaks %d",
                     (unsigned)f.proto, RT_PROTO);
            send_hello_reply(sock, f.sid, from);
            return;
        }
        if (!s_have_owner || !same_peer(&s_owner, from) || strcmp(s_sid, f.sid) != 0) {
            adopt(from, f.sid);
        }
        /* Reply to every hello, not only to the one that adopted: the app repeats it
           until answered, so a lost reply must be answerable by the next repeat. */
        send_hello_reply(sock, f.sid, from);
        return;
    }

    if (!s_have_owner || !same_peer(&s_owner, from)) return;   /* not our driver */

    /* Replay protection is what leaving TCP buys: a reordered or duplicated command
       costs one dropped datagram instead of blocking the queue behind a retransmission. */
    if (!f.has_seq) return;
    if (s_have_seq && !control_seq_newer(f.seq, s_last_seq)) return;
    s_last_seq = f.seq;
    s_have_seq = true;

    if (f.bye)          on_bye();
    else if (f.has_ty)  on_command(&f);
}

static void check_silence(void) {
    if (!s_armed || !watchdog_stale(s_last_feed_ms, now_ms(), RT_WATCHDOG_MS)) return;
    ESP_LOGW(TAG, "no control frame for >%dms — the driver is gone", RT_WATCHDOG_MS);
    s_trips++;
    /* Revoke the dead stream's grant explicitly rather than waiting for it to lapse at
       this same instant, so the retreat is not refused by a grant that is technically
       still alive. Ownership of the *channel* is deliberately kept: a stream that
       resumes after a dropout is the same session, and would otherwise be ignored
       until the app noticed and said hello again. */
    if (!link_release(LINK_SRC_RT)) {
        ESP_LOGE(TAG, "could not revoke the dead stream's grant");
    }
    recovery_on_link_lost();
    s_armed = false;   /* disarm until traffic returns */
}

static void push_telemetry(int sock) {
    if (!s_have_owner) return;
    char buf[224];
    int n = telemetry_json(buf, sizeof(buf));
    if (n <= 0) return;
    if (sendto(sock, buf, (size_t)n, 0, (const struct sockaddr *)&s_owner, sizeof(s_owner)) < 0) {
        static uint32_t last_log;
        uint32_t t = now_ms();
        if ((uint32_t)(t - last_log) > 1000) {
            last_log = t;
            ESP_LOGW(TAG, "telemetry push failed: errno %d", errno);
        }
    }
}

static void rt_task(void *arg) {
    int sock = (int)(intptr_t)arg;

    /* Subscribed to the task watchdog because this task now holds the car's safety: if
       it stops running, nothing notices silence and nothing stops the motors. A trip
       reboots the board, which is survivable only because link_init zeroes the PCA9685
       on the way up. */
    bool twdt = esp_task_wdt_add(NULL) == ESP_OK;
    if (!twdt) ESP_LOGW(TAG, "task watchdog not available");

    uint32_t next_push = now_ms();
    for (;;) {
        if (twdt) esp_task_wdt_reset();

        /* One byte of headroom: a datagram that fills it was longer than the cap and is
           refused by the parser rather than silently truncated into a valid command. */
        char buf[RT_MAX_DATAGRAM + 1];
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &flen);
        if (n > 0 && from.sin_family == AF_INET) {
            on_datagram(sock, buf, n, &from);
        } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            ESP_LOGW(TAG, "recvfrom: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(TICK_MS));   /* do not spin on a broken socket */
        }

        check_silence();

        /* A deadline rather than a tick count: the loop runs faster than its timeout
           whenever datagrams arrive, so counting iterations would make the push rate a
           function of how hard the driver is steering. */
        if ((int32_t)(now_ms() - next_push) >= 0) {
            push_telemetry(sock);
            next_push += PUSH_MS;
            /* If something held the task up for longer than a period, step to now
               rather than firing the backlog off in one burst. */
            if ((int32_t)(now_ms() - next_push) >= 0) next_push = now_ms() + PUSH_MS;
        }
    }
}

esp_err_t rt_link_start(void) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket: errno %d", errno);
        return ESP_FAIL;
    }
    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port        = htons(RT_PORT),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind %d: errno %d", RT_PORT, errno);
        close(sock);
        return ESP_FAIL;
    }
    /* The timeout IS the tick. Without it recvfrom blocks forever and the watchdog and
       the telemetry push would each need a timer of their own again. */
    struct timeval tv = { .tv_sec = 0, .tv_usec = TICK_MS * 1000 };
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        ESP_LOGE(TAG, "SO_RCVTIMEO: errno %d", errno);
        close(sock);
        return ESP_FAIL;
    }

    /* Above the httpd task and the actuator: a control frame that waits behind an OTA
       chunk is a control frame that arrives after the deadline it was measured against. */
    if (xTaskCreate(rt_task, "rt_link", 4096, (void *)(intptr_t)sock, 6, NULL) != pdPASS) {
        close(sock);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "real-time channel on UDP %d (watchdog %dms, telemetry %dHz)",
             RT_PORT, RT_WATCHDOG_MS, RT_TELEMETRY_HZ);
    return ESP_OK;
}
