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

/* Three ASCII dots and not "…": tools/gen_dongle_fonts.sh maps 32-127 and U+0400..U+04FF, so
 * U+2026 is in neither font and would draw as nothing at all — a truncation mark that is
 * itself invisible is worse than none. */
#define ELLIPSIS        "..."
#define ELLIPSIS_GLYPHS 3

/* The byte length of the longest prefix of `s` that is at most `glyphs` characters AND at most
 * `bytes` bytes, cut only where a character starts. A byte that is not a 10xxxxxx continuation
 * begins a character — the only fact about UTF-8 either budget needs, and it is what makes
 * "cut here" and "cut mid-sequence" distinguishable at all. */
static size_t utf8_fit(const char *s, size_t glyphs, size_t bytes)
{
    size_t i = 0, g = 0, ok = 0;
    for (;;) {
        if (((unsigned char)s[i] & 0xC0) != 0x80) {   /* a boundary — the NUL is one too */
            if (g > glyphs || i > bytes) break;
            ok = i;
            if (s[i] == '\0') break;
            g++;
        }
        i++;
    }
    return ok;
}

/* For the two rows this device does NOT author: an SSID (somebody's network name, up to
 * DONGLE_SSID_MAX bytes) and the firmware version string. Both fit SCREEN_ROW_MAX as bytes and
 * neither is bounded in GLYPHS, which is the budget the glass enforces — a 32-character SSID
 * survives snprintf whole and then reaches a 21-column panel, where draw_centred pins x to 0
 * and u8g2 clips the rest: the person reads 21 characters, left-aligned, with nothing to say
 * the name continues, and the row quietly stops being centred as well.
 *
 * Cut on a character boundary rather than a byte count, because a Cyrillic name spends two
 * bytes a letter and half a sequence is not a character at all. Marked when it happens, so a
 * name that was shortened does not read as a different network. */
static void put_row_bounded(screen_t *out, int r, const char *s)
{
    char *dst = out->row[r];
    if (s == NULL) { dst[0] = '\0'; return; }

    size_t whole = utf8_fit(s, SCREEN_ROW_GLYPHS, SCREEN_ROW_MAX - 1);
    if (s[whole] == '\0') {   /* inside both budgets: nothing was left out, so say nothing */
        memcpy(dst, s, whole + 1);
        return;
    }
    size_t keep = utf8_fit(s, SCREEN_ROW_GLYPHS - ELLIPSIS_GLYPHS,
                           SCREEN_ROW_MAX - sizeof(ELLIPSIS));
    memcpy(dst, s, keep);
    memcpy(dst + keep, ELLIPSIS, sizeof(ELLIPSIS));
}

/* 0 is not a reading. wifi_sta answers 0 dBm and channel 0 when the station is not connected —
 * a documented sentinel, and the same one display.c already refuses to push into the RSSI
 * history for the reason it gives there. Printed as a number it is worse than useless: 0 dBm
 * is above the top of this radio's working range, so it reads as, and would fill a gauge to,
 * the strongest link this panel can express — at the moment there is no link at all. «Сигнал»
 * and the radio diagnostics page are precisely what a person opens DURING a dropout, so that
 * is the reading they would be shown. Spelled «нет» instead, following diag_page_fault's
 * precedent for an absent errno. */
static void fmt_dbm(char *dst, size_t n, int8_t dbm)
{
    if (dbm == 0) snprintf(dst, n, "нет");
    else snprintf(dst, n, "%d dBm", (int)dbm);
}

static void fmt_channel(char *dst, size_t n, uint8_t ch)
{
    if (ch == 0) snprintf(dst, n, "нет");
    else snprintf(dst, n, "%u", (unsigned)ch);
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
     * places, no floating point on a path a display task walks five times a second.
     *
     * Unbounded by construction here, and that is fine: ota_done/ota_total cannot exceed the
     * OTA partition size, because ota_api.c rejects any upload whose content_len is larger
     * than that partition before this row is ever built (firmware/dongle/partitions.csv: ota_0
     * and ota_1 are each 0x400000 = 4 MB, so this row never needs more than one digit of MB
     * on either side of the decimal). The bound lives in ota_api.c and partitions.csv, not
     * here — a larger partition would silently widen this row again. */
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
    put_row_bounded(out, 0, v->ssid);
    /* attempts + 1, because the field counts attempts CONSUMED and this row is an ordinal:
     * the first join is in flight with none of them spent yet, so the raw count would open at
     * «Попытка 0 из 5» and never once reach 5. Nothing to clamp — wifi_state.c enters this
     * state with the count at 0 (a fresh configuration) or 1 (a link that dropped), and the
     * attempt that takes it to attempts_max is the one that moves the state to failed, so a
     * joining station is always inside 0..attempts_max-1 and this always reads 1..5. */
    snprintf(out->row[1], SCREEN_ROW_MAX, "Попытка %u из %u",
             (unsigned)v->attempts + 1u, (unsigned)v->attempts_max);
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
    put_row_bounded(out, 0, v->ssid);

    char lvl[16], ch[8];
    fmt_dbm(lvl, sizeof(lvl), v->rssi);
    fmt_channel(ch, sizeof(ch), v->channel);
    snprintf(out->row[1], SCREEN_ROW_MAX, "%s   канал %s", lvl, ch);

    /* The gauge only when there is something to gauge. This screen is chosen from the state,
     * which is read before the radio's figures and can therefore still say «Связь» over a
     * reading taken after the link went — and screens_rssi_pct clamps the 0 dBm sentinel to a
     * FULL bar. A plain rule claims nothing, which is the truthful thing to draw for a
     * measurement that does not exist. */
    if (v->rssi != 0) {
        out->gauge = GAUGE_LEVEL;
        out->gauge_pct = screens_rssi_pct(v->rssi);
    }
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
    /* v->fw already carries its own leading 'v' (firmware/dongle/CMakeLists.txt sets
     * PROJECT_VER to "v${SEMVER}+${BUILD_NUM}"), so this shows it as-is rather than
     * prepending a second one. */
    put_row_bounded(out, 0, v->fw);
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

    /* Both figures carry the same sentinel and neither may be printed as a number: the live
     * reading is 0 when the station is down (fmt_dbm), and screens_history_min answers 0 for a
     * ring nothing has been pushed into yet — a device thirty seconds out of a reboot, which
     * would otherwise report its worst-ever signal as the best one it can hold. */
    char now[16], worst[16];
    fmt_dbm(now, sizeof(now), v->rssi);
    int8_t low = (v->history != NULL) ? screens_history_min(v->history) : 0;
    if (low == 0) snprintf(worst, sizeof(worst), "нет");
    else snprintf(worst, sizeof(worst), "%d", (int)low);
    snprintf(out->row[0], SCREEN_ROW_MAX, "%s   мин %s", now, worst);
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
    /* Unbounded by construction here too: ip_be is always DONGLE_HOST and gw_be is always an
     * address the DHCP server hands out on the same link, and both are addresses inside a
     * fixed /24 (firmware/dongle/main/usb_net.h: USB_NET_ADDR = DONGLE_HOST, USB_NET_MASK =
     * 255.255.255.0) — the bound on how wide these octets can ever get lives there, not
     * here. */
    snprintf(out->row[0], SCREEN_ROW_MAX, "Адрес  %u.%u.%u.%u",
             (unsigned)((v->ip_be >> 24) & 0xFF), (unsigned)((v->ip_be >> 16) & 0xFF),
             (unsigned)((v->ip_be >> 8) & 0xFF), (unsigned)(v->ip_be & 0xFF));
    snprintf(out->row[1], SCREEN_ROW_MAX, "Шлюз   %u.%u.%u.%u",
             (unsigned)((v->gw_be >> 24) & 0xFF), (unsigned)((v->gw_be >> 16) & 0xFF),
             (unsigned)((v->gw_be >> 8) & 0xFF), (unsigned)(v->gw_be & 0xFF));
}

static void diag_page_radio(const dongle_view_t *v, screen_t *out)
{
    /* Padded with literal spaces and not with a width specifier: printf counts BYTES, and
     * «нет» is six of them for three columns, so "%18s" would align these two rows against a
     * number the panel does not draw. */
    if (v->channel == 0) put_row(out, 0, "Канал          нет");
    else snprintf(out->row[0], SCREEN_ROW_MAX, "Канал            %u", (unsigned)v->channel);

    if (v->rssi == 0) put_row(out, 1, "Уровень        нет");
    else snprintf(out->row[1], SCREEN_ROW_MAX, "Уровень    %d dBm", (int)v->rssi);
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
        /* errno_count is a uint32_t that only resets when the errno TYPE changes, never on
         * success, so a fault that never clears counts for as long as the dongle stays up —
         * roughly 77 to 116 days of continuous failure at 10-15/s before it reaches the
         * clamp below. errno itself is a small POSIX code in practice, never triple digits
         * on this port, but clamped anyway rather than trusted to stay that way. Both are
         * bounded for display, with a trailing '+' marking a clamped count, so the row's
         * width is bounded by construction rather than by how long a fault has been
         * running. */
        int err_disp = v->last_errno;
        if (err_disp < 0) err_disp = 0;
        if (err_disp > 999) err_disp = 999;
        uint32_t count = v->errno_count;
        const char *plus = "";
        if (count > 99999u) { count = 99999u; plus = "+"; }
        snprintf(out->row[0], SCREEN_ROW_MAX, "errno %d  x%u%s", err_disp, (unsigned)count, plus);
    }

    /* uptime_s is a uint32_t seconds counter with the identical shape: unclamped, the hours
     * field would need a 6th digit past ~11.4 years of continuous uptime (100000 h *
     * 3600 s/h), which is past this row's budget. Clamped here for the same reason as
     * errno_count above — bounded by construction, not by the device rebooting often
     * enough. */
    unsigned h = (unsigned)(v->uptime_s / 3600u);
    if (h > 99999u) h = 99999u;
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
