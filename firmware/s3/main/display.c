#include <string.h>

#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board.h"
#include "display.h"
#include "display_hal.h"
#include "net_api.h"
#include "net_cfg.h"
#include "ota_api.h"
#include "relay_stats.h"
#include "screens.h"
#include "status_api.h"
#include "usb_net.h"
#include "wifi_sta.h"
#include "wifi_state.h"

static const char *TAG = "display";

/* NOTHING BELOW HAS BEEN SEEN ON GLASS. The panel had not been bought when this was written:
 * every coordinate is derived from the fonts' own metrics and the design's template, and not
 * one of them has been looked at. firmware/s3/README.md's bench table lists what is owed. */

/* --- the template, in pixels ------------------------------------------------------------
 *
 * The design's eight rules give three horizontal bands on a 128x64 panel: a word in 10x20, a
 * dithered rule beneath it, and up to two rows of 6x12. The numbers here are the only place
 * that becomes coordinates.
 *
 * Vertically, out of the two fonts' own headers rather than out of taste. Each font's data
 * begins with its bounding box, and the two numbers that matter are the height and the
 * y-offset: the glyphs occupy y_offset .. y_offset + height rows around the baseline.
 * u8g2_font_10x20_t_cyrillic is 20 with an offset of -4, so it reaches 16 rows above the
 * baseline (Ё, whose diaeresis sits above the cap) and 4 below. u8g2_font_6x12_t_cyrillic is
 * 12 with an offset of -2: 10 above, 2 below.
 *
 * That gives baselines of 16, 44 and 58, and a rule band from 22 to 32 — every band clear of
 * its neighbour by two rows, the headline's top on row 0 and the lower row's descenders on
 * row 60, three clear of the last row the panel has.
 *
 * Horizontally the rule is 92 px, and that width is not a choice made here — screens.h fixes
 * it: SCREEN_HISTORY is 46 samples "at two pixels each", so 92 is the history strip's width
 * and the rule is the same object in its other two states. Centred, it starts at x 18. */
#define PANEL_W    128
#define PANEL_H    64

#define HEAD_BASE  16   /* baseline of the 10x20 headline */
#define RULE_X     18   /* (PANEL_W - RULE_W) / 2 */
#define RULE_W     92   /* SCREEN_HISTORY * 2 */
#define RULE_Y     27   /* the plain rule's row, and the level gauge's midline */
#define GAUGE_H     5   /* the level gauge fills RULE_Y-2 .. RULE_Y+2 */
#define HIST_BASE  32   /* the history's axis; its bars grow upward from just above it */
#define HIST_H     10   /* a full-strength sample is this tall, so the strip tops out at 22 */
#define MARK_GAP    6   /* between the diagnostics page markers */
#define ROW0_BASE  44   /* baselines of the two 6x12 rows */
#define ROW1_BASE  58
#define ROW_DESCENT 2   /* rows the 6x12 font puts below its baseline (its y-offset, negated) */

/* The rule's width is screens.h's number, not this file's: change SCREEN_HISTORY and the strip
 * stops filling the rule it is supposed to be. Asserted rather than commented, so the two
 * cannot drift silently. The other two say the rule is centred, and that the lower row's
 * descenders land on the panel rather than off the bottom of it. */
_Static_assert(RULE_W == SCREEN_HISTORY * 2, "the history strip must fill the rule exactly");
_Static_assert(RULE_X * 2 + RULE_W == PANEL_W, "the rule must be centred");
_Static_assert(ROW1_BASE + ROW_DESCENT <= PANEL_H - 1, "the lower row must fit the panel");

/* --- the task's own clocks -------------------------------------------------------------- */

#define PERIOD_MS       200    /* 5 Hz */
#define SPLASH_US       (2000 * 1000LL)
#define PAGE_HOLD_US    (5000 * 1000LL)

/* Every fifth pass is one second, and two things happen on it: the RSSI history takes the
 * sample screens.h sizes its ring for, and the relay's rate window is closed.
 *
 * The rate window is a second and NOT a pass for a reason worth writing down, because the
 * obvious reading of "sample once per pass" produces a figure that is wrong-looking on glass.
 * relay_stats_sample divides packets by the window, so a 200 ms window quantises the answer to
 * five packets per second per packet: the car's own 10 Hz control stream puts two datagrams in
 * each window, give or take one, and the «Пак/с» row would flicker between 5.0, 10.0 and 15.0
 * five times a second while the link was perfectly steady. A one-second window reads 10.0 and
 * moves by 1.0. The redraw stays at 5 Hz — how often the number is DRAWN and how long it is
 * MEASURED over are separate, and only the second one had to be a second. */
#define PASSES_PER_SECOND 5

/* Where the BOOT button has paged to. PAGE_STATE means "showing the state screen", which is
 * both the resting place and where the five-second timeout returns to; 0..diag_pages-1 are the
 * diagnostics pages and diag_pages itself is «Сигнал», the last stop before the wrap. */
#define PAGE_STATE (-1)

/* wifi_sta_rssi() answers 0 when the station is not connected — a documented sentinel, not a
 * reading. Pushed into the history unchanged it would be the STRONGEST sample the strip can
 * hold (screens_rssi_pct clamps anything above -25 dBm to 100%), so a dropout would draw as a
 * full bar: the graph would claim a perfect link at exactly the moment there was none.
 * Recorded as -100 dBm instead — below the -85 dBm floor where a join stops holding — so it
 * clamps to 0% and leaves a gap in the strip. That is the page's whole job: the design's "the
 * dip that breaks a link is invisible in a single number", and a dropout is the deepest dip
 * there is. The cost is that «мин» reads -100 after any outage, a floor rather than a
 * measurement, indistinguishable from a genuinely hopeless -100 dBm — and for an instrument
 * whose question is "did the link survive", those two mean the same thing. */
#define RSSI_NO_LINK (-100)

static u8g2_t s_u8g2;

/* Ruling F5: the ring is arithmetic and belongs to the pure module, but the clock that fills
 * it belongs to the task. Nothing else may push into it. */
static screens_history_t s_history;

static int64_t s_start_us;
static int     s_page = PAGE_STATE;
static int64_t s_press_us;
static bool    s_button_down;

/* --- drawing ----------------------------------------------------------------------------- */

/* u8g2 has no dithering, so the design's dithered rule is drawn a pixel at a time. */
static void draw_dithered(int x, int y, int w)
{
    for (int i = 0; i < w; i += 2) {
        u8g2_DrawPixel(&s_u8g2, x + i, y);
    }
}

static void draw_centred(int baseline, const char *s)
{
    if (s == NULL || s[0] == '\0') return;
    /* DrawUTF8 and GetUTF8Width, never DrawStr and GetStrWidth. screens.c emits UTF-8, where
     * every Cyrillic letter is two bytes; the Str forms take each BYTE as a glyph index, which
     * compiles perfectly and renders two wrong glyphs per letter — and would measure the same
     * string at twice its true width, so the centring would be wrong as well. */
    int w = u8g2_GetUTF8Width(&s_u8g2, s);
    int x = (PANEL_W - w) / 2;
    if (x < 0) x = 0;   /* a row wider than the panel starts at the left edge rather than off it */
    u8g2_DrawUTF8(&s_u8g2, x, baseline, s);
}

/* The diagnostics pages take the rule's slot for their position markers: the rule is broken
 * into one segment per page, the current one solid and the rest dithered. Not an icon and not
 * a frame — it is the rule itself, segmented, which is why it does not need an exception to
 * the template's rule 4. */
static void draw_page_marks(const screen_t *s)
{
    int pages = s->pages;
    if (pages <= 0) return;

    int seg = (RULE_W - (pages - 1) * MARK_GAP) / pages;
    if (seg < 2) seg = 2;   /* more pages than the rule has room for: still draw something */
    int total = pages * seg + (pages - 1) * MARK_GAP;
    int x = (PANEL_W - total) / 2;
    if (x < 0) x = 0;   /* u8g2's coordinates are unsigned; a negative x would wrap, not clip */

    for (int i = 0; i < pages; i++, x += seg + MARK_GAP) {
        if (i == (int)s->page) {
            u8g2_DrawBox(&s_u8g2, x, RULE_Y - 1, seg, 3);
        } else {
            draw_dithered(x, RULE_Y, seg);
        }
    }
}

static void draw_history(void)
{
    /* The axis is drawn first and unconditionally, so an empty strip is still a rule and not a
     * blank band — a device that has just booted looks like the template, not like a fault. */
    draw_dithered(RULE_X, HIST_BASE, RULE_W);

    int8_t dbm[SCREEN_HISTORY];
    uint8_t n = screens_history_read(&s_history, dbm);
    for (uint8_t i = 0; i < n; i++) {
        int h = (HIST_H * (int)screens_rssi_pct(dbm[i])) / 100;
        if (h > 0) {
            /* Oldest at the left, two pixels each, sitting on the axis rather than over it.
             * A sample at or below the floor draws nothing at all, which is what makes a
             * dropout legible as a gap. */
            u8g2_DrawBox(&s_u8g2, RULE_X + i * 2, HIST_BASE - h, 2, h);
        }
    }
}

static void draw_rule(const screen_t *s)
{
    if (s->pages > 0) {   /* only «Диагностика» sets this */
        draw_page_marks(s);
        return;
    }

    switch (s->gauge) {
    case GAUGE_LEVEL: {
        draw_dithered(RULE_X, RULE_Y, RULE_W);   /* the track the fill is read against */
        int pct = s->gauge_pct > 100 ? 100 : (int)s->gauge_pct;
        int w = (RULE_W * pct) / 100;
        if (w > 0) u8g2_DrawBox(&s_u8g2, RULE_X, RULE_Y - GAUGE_H / 2, w, GAUGE_H);
        break;
    }
    case GAUGE_HISTORY:
        draw_history();
        break;
    case GAUGE_NONE:
    default:
        draw_dithered(RULE_X, RULE_Y, RULE_W);
        break;
    }
}

static void draw(const screen_t *s)
{
    u8g2_ClearBuffer(&s_u8g2);

    u8g2_SetFont(&s_u8g2, u8g2_font_10x20_t_cyrillic);
    draw_centred(HEAD_BASE, s->head);

    draw_rule(s);

    u8g2_SetFont(&s_u8g2, u8g2_font_6x12_t_cyrillic);
    draw_centred(ROW0_BASE, s->row[0]);
    draw_centred(ROW1_BASE, s->row[1]);

    /* The one call that touches the bus. Everything above is memory. */
    u8g2_SendBuffer(&s_u8g2);
}

/* --- the view ---------------------------------------------------------------------------- */

/* screens.c renders an address's first octet from bit 31 down — `(ip_be >> 24) & 0xFF` — and
 * its host test pins that: 0xC0A80402 must read "192.168.4.2". The value it wants is therefore
 * the address as a NUMBER with the leading octet most significant, which on this little-endian
 * target is the byte-reverse of what lwIP keeps in esp_ip4_addr_t.addr, whatever the field's
 * `_be` name suggests. Assembled octet by octet through esp_netif's own accessors rather than
 * byte-swapped, so this says which octet goes where instead of depending on the build's
 * endianness to make it come out right. */
static uint32_t view_addr(const esp_ip4_addr_t *a)
{
    return ((uint32_t)esp_ip4_addr1_16(a) << 24) |
           ((uint32_t)esp_ip4_addr2_16(a) << 16) |
           ((uint32_t)esp_ip4_addr3_16(a) << 8)  |
           ((uint32_t)esp_ip4_addr4_16(a));
}

static void view_build(dongle_view_t *v, net_cfg_t *cfg, const esp_app_desc_t *app,
                       int64_t now_us)
{
    memset(v, 0, sizeof(*v));

    /* The station's state FIRST, the radio's figures after. status_api.c's status_get()
     * carries the twin of this comment for the same reason, and this is the caller that made
     * it matter: wifi_sta_channel() and wifi_sta_rssi() are unlocked esp_wifi_sta_get_ap_info
     * calls rather than reads of wifi_sta's lock-free mirror, so {state, rssi, channel} is not
     * one snapshot — and /status only ever paired two of them. Reading the state first leaves
     * only the pessimistic half of the race: a «Связь» screen over a figure that went stale
     * between the two reads, never a live figure under a state that had already dropped. */
    v->state = wifi_sta_state_name();
    v->rssi = wifi_sta_rssi();
    v->channel = wifi_sta_channel();
    v->attempts = wifi_sta_attempts();
    v->attempts_max = WIFI_JOIN_ATTEMPTS;

    v->host_attached = usb_net_host_attached();
    v->ssid = net_api_current(cfg) ? cfg->ssid : NULL;
    v->fw = app->version;
    v->rolled_back = status_api_rolled_back();

    uint32_t done = 0, total = 0;
    v->ota_active = ota_api_progress(&done, &total);
    v->ota_done = done;
    v->ota_total = total;

    /* Both addresses out of ONE esp_netif snapshot, rather than the address from here and the
     * gateway from wifi_sta_gateway(). They are shown one above the other on the same page, and
     * two sources can disagree: wifi_sta_gateway keeps the last-known gateway across a drop on
     * purpose (it is the relays' destination and a softAP's gateway does not move), which
     * beside a zeroed address would read as a link that half exists. It also takes wifi_sta's
     * bounded-wait lock and logs an ESP_LOGE whenever that lock is busy — the right trade for a
     * relay that needs a destination, the wrong one for a cosmetic read five times a second. */
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip;
    if (sta != NULL && esp_netif_get_ip_info(sta, &ip) == ESP_OK) {
        v->ip_be = view_addr(&ip.ip);
        v->gw_be = view_addr(&ip.gw);
    }

    /* Read field by field, not copied wholesale: relay_stats.h documents these as unlocked on
     * purpose, and a memcpy would not make them one snapshot either. The only pair that can
     * tear is errno/errno_count, and that is accepted there for a reason this file does not get
     * to overrule — the forwarding path must never wait. */
    const relay_stats_t *r = relay_stats_shared();
    v->to_car_x10 = r->to_car_x10;
    v->to_phone_x10 = r->to_phone_x10;
    v->udp_used = r->udp_used;
    v->tcp_used = r->tcp_used;
    v->last_errno = r->last_errno;
    v->errno_count = r->errno_count;

    v->uptime_s = (uint32_t)(now_us / 1000000);

    v->history = &s_history;
}

/* --- the BOOT button --------------------------------------------------------------------- */

/* Polled once a pass and no more. The 200 ms between samples IS the debounce: a press only
 * counts on a low sample that follows a high one, so a contact bouncing for a millisecond
 * cannot be seen twice — there is no window in which a second edge could land. No interrupt,
 * deliberately: an ISR on GPIO0 would buy nothing a button needs and would put a handler on
 * the chip's own boot strap.
 *
 * The honest cost of sampling that slowly: a press and release that both fall between two
 * samples is not seen at all, and a deliberate press is usually longer than 200 ms but not
 * always. The remedy is to press again, which is what a person does with a page button anyway
 * — but it is a real property of this loop and not a theoretical one, and it has never been
 * tried on hardware. */
static void poll_button(int64_t now_us)
{
    bool down = gpio_get_level(BOARD_BOOT_GPIO) == 0;   /* pulled up; pressed is low */

    if (down && !s_button_down) {
        int last = (int)screens_diag_pages();   /* the diagnostics run 0..last-1, «Сигнал» is last */
        s_page = (s_page >= last) ? PAGE_STATE : s_page + 1;
        s_press_us = now_us;
    }
    s_button_down = down;

    /* Back to the state screen five seconds after the last press. The reference pages are
     * something a person went looking for; the state screen is what the device is for. */
    if (s_page != PAGE_STATE && now_us - s_press_us > PAGE_HOLD_US) {
        s_page = PAGE_STATE;
    }
}

/* --- the task ---------------------------------------------------------------------------- */

static void display_task(void *arg)
{
    (void)arg;

    const esp_app_desc_t *app = esp_app_get_description();
    TickType_t last_wake = xTaskGetTickCount();
    uint32_t tick = 0;

    for (;;) {
        int64_t now_us = esp_timer_get_time();
        bool second = (tick % PASSES_PER_SECOND) == 0;

        /* Closes the rate window relay_stats.h describes, before the view reads what it
         * latched. This task is its ONLY caller, so GET /status's to_car_x10 / to_phone_x10
         * are latched here and nowhere else — they read 0 for the whole life of a build in
         * which this task does not run. */
        if (second) relay_stats_sample(relay_stats_shared(), (uint32_t)(now_us / 1000));

        poll_button(now_us);

        net_cfg_t cfg;
        dongle_view_t v;
        view_build(&v, &cfg, app, now_us);

        if (second) screens_history_push(&s_history, v.rssi != 0 ? v.rssi : RSSI_NO_LINK);

        screen_t s;
        if (now_us - s_start_us < SPLASH_US) {
            /* The splash is the one screen that is not a report. For two seconds the device
             * introduces itself, and the design's point about it is that "if it appears, power,
             * I2C and the firmware itself are alive" — which is only true if it appears
             * unconditionally. screens_for reaches it through its own unrecognised-state
             * fallback, so what goes in is a view that asks for the introduction and carries
             * nothing else: no state to recognise, no OTA, no rollback, and host_attached true
             * so that «Нет хоста», which outranks everything, does not take the two seconds for
             * itself on a bench-powered board. */
            dongle_view_t intro = { .host_attached = true, .fw = v.fw };
            screens_for(&intro, &s);
        } else if (s_page == PAGE_STATE) {
            screens_for(&v, &s);
        } else if (s_page < (int)screens_diag_pages()) {
            screens_diag(&v, (uint8_t)s_page, &s);
        } else {
            screens_signal(&v, &s);
        }

        draw(&s);

        tick++;
        /* vTaskDelayUntil, so a slow pass — an I2C bus holding the line, say — costs cadence
         * and not drift. */
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(PERIOD_MS));
    }
}

esp_err_t display_start(void)
{
    screens_history_init(&s_history);
    s_start_us = esp_timer_get_time();
    s_press_us = s_start_us;

    gpio_config_t btn = {
        .pin_bit_mask = 1ULL << BOARD_BOOT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&btn);
    if (err != ESP_OK) {
        /* Not fatal and not returned: the state screens are the device's job and they do not
         * need a button. Only the reference pages become unreachable. */
        ESP_LOGE(TAG, "BOOT (GPIO%d) would not become an input (%s) — the reference pages "
                      "cannot be paged to", (int)BOARD_BOOT_GPIO, esp_err_to_name(err));
    }

    err = display_hal_setup(&s_u8g2);
    if (err == ESP_OK) {
        u8g2_InitDisplay(&s_u8g2);
        u8g2_SetPowerSave(&s_u8g2, 0);
    }
    /* An error is logged by display_hal and not returned. The task starts either way: drawing
     * into u8g2's RAM buffer costs nothing when the bus discards it, and this task also carries
     * relay_stats_sample() — a panel nobody wired must not take /status's packet rates with it. */

    /* Priority 2, below the relays' 5: a redraw must never preempt a task that is forwarding a
     * control datagram. Everything this task does is memory, one I2C burst and a screen nobody
     * is looking at most of the time — it is the least load-bearing thing on the board and its
     * priority says so. */
    if (xTaskCreate(display_task, "display", 4096, NULL, 2, NULL) != pdPASS) {
        ESP_LOGE(TAG, "display task would not start");
        return ESP_FAIL;
    }
    return ESP_OK;
}
