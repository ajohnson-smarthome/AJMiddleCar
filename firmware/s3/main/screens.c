#include <stdio.h>
#include <string.h>

#include "screens.h"

/* --- history ------------------------------------------------------------------------- */

void screens_history_init(screens_history_t *h)
{
    memset(h, 0, sizeof(*h));
}

void screens_history_push(screens_history_t *h, int8_t dbm)
{
    h->dbm[h->next] = dbm;
    h->next = (uint8_t)((h->next + 1) % SCREEN_HISTORY);
    if (h->count < SCREEN_HISTORY) h->count++;
}

uint8_t screens_history_read(const screens_history_t *h, int8_t *out)
{
    /* `next` is also where the OLDEST sample sits once the ring has wrapped (the slot the
     * next push will overwrite is the one furthest in the past); before it wraps, the oldest
     * is simply index 0 and `next` is one past the newest. Both cases fall out of the same
     * formula because count < SCREEN_HISTORY makes `next - count` land on 0 or negative-wrapped
     * to the same place index 0 would. */
    uint8_t start = (uint8_t)((h->next + SCREEN_HISTORY - h->count) % SCREEN_HISTORY);
    for (uint8_t i = 0; i < h->count; i++)
        out[i] = h->dbm[(start + i) % SCREEN_HISTORY];
    return h->count;
}

int8_t screens_history_min(const screens_history_t *h)
{
    if (h->count == 0) return 0;
    int8_t m = h->dbm[0];
    for (uint8_t i = 1; i < h->count; i++)
        if (h->dbm[i] < m) m = h->dbm[i];
    return m;
}

/* --- signal strength ------------------------------------------------------------------ */

uint8_t screens_rssi_pct(int8_t dbm)
{
    /* -85 dBm is where a join stops holding on this radio; -25 dBm is desk distance — the
     * strongest reading this dongle sees in practice. Both ends clamp so a reading outside
     * that working range still renders as a sensible 0..100 rather than going negative or
     * past full. */
    const int lo = -85, hi = -25;
    int v = dbm;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return (uint8_t)(((v - lo) * 100) / (hi - lo));
}

/* --- small formatting helpers ---------------------------------------------------------- */

static void put_head(screen_t *out, const char *s)
{
    snprintf(out->head, SCREEN_HEAD_MAX, "%s", s);
}

static void put_row(screen_t *out, int r, const char *s)
{
    snprintf(out->row[r], SCREEN_ROW_MAX, "%s", s);
}

/* --- screens_for: the state precedence -------------------------------------------------- */

static void fill_no_host(screen_t *out)
{
    out->id = SCREEN_NO_HOST;
    put_head(out, "Нет хоста");
    put_row(out, 0, "Питание от COM");
    put_row(out, 1, "Стендовый режим");
}

static void fill_updating(const dongle_view_t *v, screen_t *out)
{
    out->id = SCREEN_UPDATING;
    put_head(out, "Обновление");

    /* Percent complete, guarded against a total of zero (an OTA that has announced itself
     * but not yet learned its own size). Widened to 64 bits for the multiply: done and total
     * are each up to a uint32_t, and done*100 alone can already exceed what a 32-bit product
     * holds. */
    unsigned pct = 0;
    if (v->ota_total > 0) {
        uint64_t p = ((uint64_t)v->ota_done * 100) / v->ota_total;
        pct = (p > 100) ? 100 : (unsigned)p;
    }
    out->gauge = GAUGE_LEVEL;
    out->gauge_pct = (uint8_t)pct;

    /* Megabytes as this project spells them elsewhere — decimal (bytes / 1e6), two decimal
     * places, no floating point on a path a display task walks five times a second. */
    unsigned done_mb = (unsigned)(v->ota_done / 1000000u);
    unsigned done_cs = (unsigned)((v->ota_done % 1000000u) / 10000u);
    unsigned total_mb = (unsigned)(v->ota_total / 1000000u);
    unsigned total_cs = (unsigned)((v->ota_total % 1000000u) / 10000u);
    snprintf(out->row[0], SCREEN_ROW_MAX, "%u%%   %u.%02u/%u.%02u МБ",
             pct, done_mb, done_cs, total_mb, total_cs);
    put_row(out, 1, "");
}

static void fill_rolled_back(screen_t *out)
{
    out->id = SCREEN_ROLLED_BACK;
    put_head(out, "Откат");
    put_row(out, 0, "Прежняя версия");
    put_row(out, 1, "Нужен новый выпуск");
}

static void fill_unconfigured(screen_t *out)
{
    out->id = SCREEN_UNCONFIGURED;
    put_head(out, "Не настроен");
    put_row(out, 0, "Сеть не задана");
    put_row(out, 1, "Задаёт приложение");
}

static void fill_searching(const dongle_view_t *v, screen_t *out)
{
    out->id = SCREEN_SEARCHING;
    put_head(out, "Поиск сети");
    put_row(out, 0, v->ssid ? v->ssid : "");
    snprintf(out->row[1], SCREEN_ROW_MAX, "Попытка %u из %u",
             (unsigned)v->attempts, (unsigned)v->attempts_max);
}

static void fill_joining(screen_t *out)
{
    out->id = SCREEN_JOINING;
    put_head(out, "Подключение");
    put_row(out, 0, "Сеть найдена");
    put_row(out, 1, "Получение адреса");
}

static void fill_linked(const dongle_view_t *v, screen_t *out)
{
    out->id = SCREEN_LINKED;
    put_head(out, "Связь");
    put_row(out, 0, v->ssid ? v->ssid : "");
    snprintf(out->row[1], SCREEN_ROW_MAX, "%d dBm   канал %u",
             (int)v->rssi, (unsigned)v->channel);
    out->gauge = GAUGE_LEVEL;
    out->gauge_pct = screens_rssi_pct(v->rssi);
}

static void fill_no_network(screen_t *out)
{
    out->id = SCREEN_NO_NETWORK;
    put_head(out, "Нет сети");
    put_row(out, 0, "Попытки исчерпаны");
    put_row(out, 1, "Проверьте машинку");
}

static void fill_splash(const dongle_view_t *v, screen_t *out)
{
    out->id = SCREEN_SPLASH;
    put_head(out, "AJDONGLE");
    /* v->fw already carries its own leading 'v' (firmware/s3/CMakeLists.txt sets
     * PROJECT_VER to "v${SEMVER}+${BUILD_NUM}"), so this shows it as-is rather than
     * prepending a second one. */
    put_row(out, 0, v->fw ? v->fw : "");
    put_row(out, 1, "");
}

void screens_for(const dongle_view_t *v, screen_t *out)
{
    memset(out, 0, sizeof(*out));
    out->gauge = GAUGE_NONE;

    /* Highest first: no USB host, then an OTA in flight, then a rolled-back image, then the
     * station's own state. A dongle with no host attached has nothing to say about a network
     * — there is no phone there to read it — so that check runs first and unconditionally. */
    if (!v->host_attached) { fill_no_host(out); return; }
    if (v->ota_active) { fill_updating(v, out); return; }
    if (v->rolled_back) { fill_rolled_back(out); return; }

    if (v->state != NULL) {
        if (strcmp(v->state, DONGLE_STATE_IDLE) == 0) { fill_unconfigured(out); return; }
        if (strcmp(v->state, DONGLE_STATE_SEARCHING) == 0) { fill_searching(v, out); return; }
        if (strcmp(v->state, DONGLE_STATE_JOINING) == 0) { fill_joining(out); return; }
        if (strcmp(v->state, DONGLE_STATE_CONNECTED) == 0) { fill_linked(v, out); return; }
        if (strcmp(v->state, DONGLE_STATE_FAILED) == 0) { fill_no_network(out); return; }
    }

    /* A state this build does not recognise — including no state read yet at all — falls
     * back to introducing the device rather than guessing at a network. */
    fill_splash(v, out);
}

/* --- screens_signal: reached only by paging with BOOT ----------------------------------- */

void screens_signal(const dongle_view_t *v, screen_t *out)
{
    memset(out, 0, sizeof(*out));
    out->id = SCREEN_SIGNAL;
    put_head(out, "Сигнал");
    int8_t worst = v->history ? screens_history_min(v->history) : 0;
    snprintf(out->row[0], SCREEN_ROW_MAX, "%d dBm   мин %d",
             (int)v->rssi, (int)worst);
    put_row(out, 1, "");
    out->gauge = GAUGE_HISTORY;
}

/* --- screens_diag: reached only by paging with BOOT -------------------------------------- */

#define SCREENS_DIAG_PAGES 4

uint8_t screens_diag_pages(void)
{
    return SCREENS_DIAG_PAGES;
}

static void diag_page_address(const dongle_view_t *v, screen_t *out)
{
    snprintf(out->row[0], SCREEN_ROW_MAX, "Адрес  %u.%u.%u.%u",
             (unsigned)((v->ip_be >> 24) & 0xFF), (unsigned)((v->ip_be >> 16) & 0xFF),
             (unsigned)((v->ip_be >> 8) & 0xFF), (unsigned)(v->ip_be & 0xFF));
    snprintf(out->row[1], SCREEN_ROW_MAX, "Шлюз   %u.%u.%u.%u",
             (unsigned)((v->gw_be >> 24) & 0xFF), (unsigned)((v->gw_be >> 16) & 0xFF),
             (unsigned)((v->gw_be >> 8) & 0xFF), (unsigned)(v->gw_be & 0xFF));
}

static void diag_page_radio(const dongle_view_t *v, screen_t *out)
{
    snprintf(out->row[0], SCREEN_ROW_MAX, "Канал            %u", (unsigned)v->channel);
    snprintf(out->row[1], SCREEN_ROW_MAX, "Уровень    %d dBm", (int)v->rssi);
}

static void diag_page_relay(const dongle_view_t *v, screen_t *out)
{
    snprintf(out->row[0], SCREEN_ROW_MAX, "Слоты  TCP %u UDP %u",
             (unsigned)v->tcp_used, (unsigned)v->udp_used);

    /* to_car_x10 and to_phone_x10 are each a uint16_t and can reach 6553.5, but the real
     * ceiling is roughly 40/s — four phone sessions at 10 Hz each. A figure above 99.9 means
     * something is badly wrong, and its exact value has stopped being the interesting thing:
     * rendered at 99.9 instead, so the row's width is bounded by construction rather than by
     * hoping the numbers stay small. "Пак/с" (packets per second) rather than the bare label
     * this row used to carry — without a unit, a rate reads just as easily as a total. */
    unsigned to_car = (v->to_car_x10 > 999) ? 999 : (unsigned)v->to_car_x10;
    unsigned to_phone = (v->to_phone_x10 > 999) ? 999 : (unsigned)v->to_phone_x10;
    snprintf(out->row[1], SCREEN_ROW_MAX, "Пак/с  %u.%u / %u.%u",
             to_car / 10, to_car % 10, to_phone / 10, to_phone % 10);
}

static void diag_page_fault(const dongle_view_t *v, screen_t *out)
{
    if (v->last_errno == 0) {
        /* No fault reads as "none" — "нет" — never as the digit 0, which would read as an
         * errno of zero rather than the absence of one. */
        put_row(out, 0, "errno          нет");
    } else {
        snprintf(out->row[0], SCREEN_ROW_MAX, "errno %d     x%u",
                 v->last_errno, (unsigned)v->errno_count);
    }
    unsigned h = (unsigned)(v->uptime_s / 3600u);
    unsigned m = (unsigned)((v->uptime_s / 60u) % 60u);
    unsigned s = (unsigned)(v->uptime_s % 60u);
    snprintf(out->row[1], SCREEN_ROW_MAX, "Аптайм    %02u:%02u:%02u", h, m, s);
}

void screens_diag(const dongle_view_t *v, uint8_t page, screen_t *out)
{
    memset(out, 0, sizeof(*out));
    out->id = SCREEN_DIAG;
    put_head(out, "Диагностика");
    out->pages = SCREENS_DIAG_PAGES;
    out->page = page;

    switch (page) {
    case 0: diag_page_address(v, out); break;
    case 1: diag_page_radio(v, out); break;
    case 2: diag_page_relay(v, out); break;
    default: diag_page_fault(v, out); break;
    }
}
