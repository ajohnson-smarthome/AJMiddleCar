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
#include "rt_glue.h"

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
   is a hello, and a hello always wins.

   The address lives here because it is the only part of the session that needs lwip;
   the rest — the sid, the sequence gate, the control watchdog's arm flag and deadline —
   is `rt_session_t` in rt_link.h, pure and host-tested. */
static struct sockaddr_in s_owner;
static rt_session_t       s_ses;
static rt_dead_sids_t     s_dead;

/* The real effects table — every entry a one-line adapter, so the orderings above it
   are exactly the host-tested ones in rt_glue.h. */
static bool fx_stop_safe(void *c)    { (void)c; return car_stop(LINK_SRC_SAFE); }
static bool fx_release_safe(void *c) { (void)c; return link_release_must(LINK_SRC_SAFE); }
static bool fx_release_rt(void *c)   { (void)c; return link_release_must(LINK_SRC_RT); }
static void fx_forget(void *c)       { (void)c; recovery_forget(); }
static void fx_on_lost(void *c)      { (void)c; recovery_on_link_lost(); }
static link_src_t fx_owner(void *c)  { (void)c; return link_owner(); }
static const rt_effects_t FX = { NULL, fx_stop_safe, fx_release_safe, fx_release_rt,
                                 fx_forget, fx_on_lost, fx_owner };

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
    s_owner = *from;
    /* Ordering and rationale live in rt_glue_adopt, where they are host-tested. */
    if (!rt_glue_adopt(&s_ses, &s_dead, sid, now_ms(), &FX)) {
        ESP_LOGE(TAG, "adopt could not release a leftover SAFE grant");
    }
    log_peer("session adopted from", from);
}

static void on_bye(void) {
    switch (rt_glue_bye(&s_ses, &s_dead, &FX)) {
    case RT_BYE_PLAIN:
        ESP_LOGI(TAG, "goodbye — stopping, and not retreating");
        break;
    case RT_BYE_UNDER_STICKY:
        /* OTA or the wizard owns the motors; the goodbye ends the session and the
           breadcrumbs, and keeps its hands off the sticky grant — grabbing SAFE over
           a flash re-opened the actuator for the rest of the write. */
        ESP_LOGI(TAG, "goodbye during %s — session over, actuator untouched",
                 link_src_name(link_owner()));
        break;
    case RT_BYE_STOP_REFUSED:
        ESP_LOGE(TAG, "goodbye stop was not applied — %s holds the actuator",
                 link_src_name(link_owner()));
        break;
    }
}

static void on_command(const control_frame_t *f) {
    s_frames++;
    /* The first accepted command arms the watchdog; every one after it refreshes the
       deadline. The breadcrumb IS gated on the grant: a refused command never moved the
       car, so recording it would corrupt the path the retreat retraces. */
    rt_session_command(&s_ses, f->seq, now_ms());
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

    /* Every rule about who this datagram is from, whether it can be ordered and what it
       asks for is in rt_session_classify — pure, and host-tested in test_rt_session.c.
       What is left here is the part that needs a socket and an actuator. */
    switch (rt_session_classify(&s_ses, &s_dead, s_ses.have_owner && same_peer(&s_owner, from), &f)) {
    case RT_ADOPT:
        adopt(from, f.sid);
        send_hello_reply(sock, f.sid, from);
        break;
    case RT_REPLY:
        /* The forced-update gate exists to make a mismatch brief, but it only works if
           the mismatch is visible: the reply names our version. Today's app discards a
           reply whose proto is not its own, so this line is for the log and for
           whatever reads it next. */
        if (!f.has_proto || f.proto != RT_PROTO) {
            ESP_LOGW(TAG, "hello with proto %u, this car speaks %d",
                     (unsigned)f.proto, RT_PROTO);
        }
        /* Answered whether or not it adopted: the app repeats the handshake until it is
           answered, so a lost reply must be answerable by the next repeat. */
        send_hello_reply(sock, f.sid, from);
        break;
    case RT_BYE:
        on_bye();
        break;
    case RT_COMMAND:
        on_command(&f);
        break;
    case RT_DROP:
        break;
    }
}

static void check_silence(void) {
    if (!rt_glue_silence(&s_ses, now_ms(), &FX)) return;
    s_trips++;
    ESP_LOGW(TAG, "no control frame for >%dms — the driver is gone", RT_WATCHDOG_MS);
}

static void check_idle(void) {
    if (!rt_glue_idle(&s_ses, &s_dead, now_ms(), &FX)) return;
    ESP_LOGI(TAG, "session idle for >%dms — over; the next driver says hello",
             RT_SESSION_IDLE_MS);
}

static void push_telemetry(int sock) {
    if (!s_ses.have_owner) return;
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
        check_idle();

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
