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
#include "contract.h"
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

/* How many datagrams the loop will take before it yields whatever it is still holding.
   With traffic pending, recvfrom returns immediately and this task — the highest-priority
   one in the application — never blocks, so a flood on port RT_PORT would starve the
   actuator task (20 ms deadline) and the idle task while this loop kept its own task
   watchdog happy. Eight per yield is two orders of magnitude above the RT_COMMAND_HZ
   the wire actually carries. */
#define BURST_MAX 8

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
   until a session exists, so a car nobody has connected to never "loses" a link it
   never had; adoption arms it, and a goodbye disarms it again. */
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
                     "{\"" RT_KEY_PROTO "\":%d,\"" RT_KEY_HELLO "\":\"%s\","
                     "\"" RT_KEY_DEVICE "\":\"" CAR_DEVICE_ID "\",\"" RT_KEY_FW "\":\"%s\"}",
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
    /* Armed by the handshake, not by the first command. A session that is adopted and
       then goes quiet is exactly as lost as one that stops mid-drive, and the app
       reconnects with a fresh sid well inside the deadline — arming on the first
       command let a reconnect erase a loss that had already happened. A trip with no
       breadcrumbs behind it degrades to a plain stop, so the wrong-car flow (hello,
       foreign device, goodbye) costs nothing. */
    s_last_feed_ms = now_ms();
    s_armed        = true;
    snprintf(s_sid, sizeof(s_sid), "%s", sid);
    /* A previous session's goodbye left SAFE holding zero, deliberately: nothing may
       command the motors between a goodbye and the next driver identifying themselves.
       This is that moment, so release it — and with it the console and the wizard. */
    link_release(LINK_SRC_SAFE);
    log_peer("session adopted from", from);
}

static void on_bye(void) {
    ESP_LOGI(TAG, "goodbye — stopping, and not retreating");
    /* First, in case a retreat is already running from an earlier dropout: a frame from
       the driver is what ends one, and this frame says stop. Recording it also leaves a
       stationary newest breadcrumb, so the next retreat starts from a car at rest. */
    recovery_note_command(0.0f, 0.0f);
    /* Deliberately NOT released here. SAFE is sticky, so the stop holds until the next
       adopt(): a retreat already past the seq check in its wait loop would otherwise
       find the actuator free and land one reverse step — up to an actuator tick of the
       car moving backwards after the driver said stop, which is the one thing a goodbye
       exists to prevent. link_set writes nothing on a lock timeout, so a stop that was
       not applied is worth a line: the grant lapse still stops the car within
       RT_WATCHDOG_MS, but not because we asked. */
    if (!car_stop(LINK_SRC_SAFE)) {
        ESP_LOGE(TAG, "goodbye stop was not applied — %s holds the actuator",
                 link_src_name(link_owner()));
    }
    /* This is the whole point of carrying a goodbye on the wire: silence that was
       announced is not silence that means the driver is out of range. */
    s_armed      = false;
    s_have_owner = false;
    s_have_seq   = false;
}

static void on_command(const control_frame_t *f) {
    s_frames++;
    /* A frame refreshes the deadline the handshake started. That is the only thing this
       watchdog measures — actuator health is a separate question, answered by bus_ok. The
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
    /* The COMMAND cap, not the datagram cap: RT_MAX_DATAGRAM sizes a receive buffer on
       both sides (telemetry is the wide direction), while RT_MAX_COMMAND is the largest
       thing the car will accept. Anything bigger is refused here rather than parsed. */
    if (control_parse_frame(buf, (size_t)n, RT_MAX_COMMAND, &f) != 0) {
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
            /* Answer anyway, but do not adopt: a session neither side can parse is
               worse than no session, and the forced-update gate exists to make this
               state brief. The reply names our version so that a mismatch is visible
               at all — today's app discards a reply whose proto is not its own, so
               this is for the log and for whatever reads it next, not for the app. */
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
    /* Silence past the deadline already proves the stream is dead, so there is nothing
       left to replay-protect — and a seq gate left desynchronised (one spoofed frame
       far in the future, or a counter bug) would drop every genuine frame for the rest
       of the session while telemetry kept flowing and the app showed a healthy link. */
    s_have_seq = false;
}

static void push_telemetry(int sock) {
    if (!s_have_owner) return;
    /* The wide direction of the wire: a telemetry frame runs to ~160 bytes, which is
       why the schema's receive cap (RT_MAX_DATAGRAM) is not the command cap. */
    char buf[RT_MAX_DATAGRAM];
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
    int burst = 0;
    for (;;) {
        if (twdt) esp_task_wdt_reset();

        /* Sized from the receive cap, which is wider than anything the car accepts:
           an oversized datagram arrives whole, is measured, and is refused by the
           parser rather than silently truncated into a valid command. */
        char buf[RT_MAX_DATAGRAM];
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &flen);
        if (n > 0 && from.sin_family == AF_INET) {
            on_datagram(sock, buf, n, &from);
            /* Yield after a burst: see BURST_MAX. A tick here costs the wire nothing
               and is the difference between a flood being noisy and a flood rebooting
               the board through the actuator task's watchdog. */
            if (++burst >= BURST_MAX) {
                burst = 0;
                vTaskDelay(1);
            }
        } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            ESP_LOGW(TAG, "recvfrom: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(TICK_MS));   /* do not spin on a broken socket */
            burst = 0;
        } else {
            burst = 0;   /* the timeout fired: the loop is keeping up on its own */
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
