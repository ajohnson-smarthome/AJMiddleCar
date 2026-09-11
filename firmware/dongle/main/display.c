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
 * one of them has been looked at. firmware/dongle/README.md's bench table lists what is owed. */

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
 * ONE THING THAT WILL LOOK LIKE A BUG ON THE BENCH and is not: the rule does not sit at the
 * same y on «Сигнал» as everywhere else. The plain rule and the level gauge are single lines
 * and are centred IN the band, at row 27; the history is a strip that needs the whole band, so
 * it hangs from the band's floor at row 32 and grows up to its ceiling at 22. Both are the same
 * object occupying the same band — but a one-row line centred in an eleven-row band and an
 * eleven-row strip filling it cannot share a y, and paging from «Связь» to «Сигнал» therefore
 * drops the line five rows. Making them share a y would mean either a history with half the
 * headroom or a plain rule sitting off-centre on nine screens out of eleven.
 *
 * Horizontally the rule is 92 px, and that width is not a choice made here — screens.h fixes
 * it: SCREEN_HISTORY is 46 samples "at two pixels each", so 92 is the history strip's width
 * and the rule is the same object in its other two states. Centred, it starts at x 18. */
#define PANEL_W    128
#define PANEL_H    64

#define HEAD_BASE  16   /* baseline of the 10x20 headline */
#define HEAD_ASCENT 16  /* rows the 10x20 font can reach above its baseline (height + y-offset) */
#define HEAD_DESCENT 4  /* ... and below it (its y-offset, negated) */
#define RULE_X     18   /* (PANEL_W - RULE_W) / 2 */
#define RULE_W     92   /* SCREEN_HISTORY * 2 */
#define RULE_Y     27   /* the plain rule's row, and the level gauge's midline */
#define GAUGE_H     5   /* the level gauge fills RULE_Y-2 .. RULE_Y+2 */
#define HIST_TOP   22   /* the rule band's own top edge: where a full-strength sample reaches */
#define HIST_BASE  32   /* the history's axis; its bars grow upward from just above it */
#define HIST_H     (HIST_BASE - HIST_TOP)   /* 10 rows of headroom for a full sample */
#define MARK_GAP    6   /* between the diagnostics page markers */
#define ROW0_BASE  44   /* baselines of the two 6x12 rows */
#define ROW1_BASE  58
#define ROW_ASCENT  10  /* rows the 6x12 font can reach above its baseline (height + y-offset) */
#define ROW_DESCENT 2   /* ... and below it (its y-offset, negated) */

/* Every constraint the prose above states, stated again where the compiler can check it — the
 * whole layout rather than the parts that were easy to assert. The first ties the rule's width
 * to screens.h (change SCREEN_HISTORY and the strip stops filling the rule it is supposed to
 * be); the rest keep each band on the panel and clear of its neighbour. */
_Static_assert(RULE_W == SCREEN_HISTORY * 2, "the history strip must fill the rule exactly");
_Static_assert(RULE_X * 2 + RULE_W == PANEL_W, "the rule must be centred");
_Static_assert(HEAD_BASE - HEAD_ASCENT >= 0, "the headline's tallest glyph must fit above it");
_Static_assert(HEAD_BASE + HEAD_DESCENT < HIST_TOP, "the headline must clear the rule band");
_Static_assert(HIST_BASE < ROW0_BASE - ROW_ASCENT, "the rule band must clear the upper row");
_Static_assert(ROW0_BASE + ROW_DESCENT < ROW1_BASE - ROW_ASCENT, "the two rows must not touch");
_Static_assert(ROW1_BASE + ROW_DESCENT <= PANEL_H - 1, "the lower row must fit the panel");

/* --- the task's own clocks -------------------------------------------------------------- */

#define PERIOD_MS       200    /* 5 Hz */
#define SPLASH_US       (2000 * 1000LL)
#define PAGE_HOLD_US    (5000 * 1000LL)

/* One second, and two things happen on it: the RSSI history takes the sample screens.h sizes
 * its ring for, and the relay's rate window is closed.
 *
 * Off the clock, not off a count of passes. vTaskDelayUntil holds the cadence while passes are
 * quick, but a pass that runs long does not shorten the next one back into line, and
 * display_hal.c budgets for exactly that: a wedged I2C bus costs 64 transfers of 50 ms, a frame
 * every 3.2 s. Every fifth pass would then be sixteen seconds. relay_stats_sample divides by
 * the elapsed milliseconds it is handed, so the RATE would stay honest right through that while
 * the history's time base stretched underneath it — a strip drawn as 46 seconds of link that
 * was really twelve minutes of one, and no way to tell from the glass. One clock for both, so
 * the two cannot disagree about how long a second is.
 *
 * Why a second at all and not a pass: relay_stats_sample divides packets by the window, so a
 * 200 ms window quantises the answer to five packets per second per packet — the car's own
 * 10 Hz control stream puts two datagrams in each window, give or take one, and the «Пак/с» row
 * would flicker between 5.0, 10.0 and 15.0 five times a second while the link was perfectly
 * steady. A one-second window reads 10.0 and moves by 1.0. The redraw stays at 5 Hz: how often
 * the number is DRAWN and how long it is MEASURED over are separate, and only the second one
 * had to be a second. */
#define SECOND_US (1000 * 1000LL)


/* wifi_sta_ap_info() answers 0 dBm when the station is not connected — a documented sentinel,
 * not a reading. Pushed into the history unchanged it would be the STRONGEST sample the strip can
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

/* Resolved once in display_start(). The reason it is not looked up per pass is written at the
 * point of use in view_build(), where the temptation to reach for the lookup lives. */
static esp_netif_t *s_sta_netif;

static int64_t s_start_us;
static int     s_page = SCREENS_PAGE_STATE;
static int64_t s_press_us;
static bool    s_button_down;

/* False until gpio_config() has actually made BOOT an input. display_start() logs a failure
 * there and carries on — the state screens are the device's job and do not need a button — but
 * "carries on" has to mean the pin is never read, not that it is read anyway. gpio_get_level()
 * on a pad this firmware never configured returns whatever the strapping left it at, and one
 * value of that is indistinguishable from a press: the first pass would see an edge, page to
 * «Диагностика», and the device would open on a reference page for five seconds after every
 * boot. This flag is what makes display_start's "only the reference pages become unreachable"
 * true rather than aspirational. */
static bool    s_button_ok;

/* The frame the panel is actually showing, so an identical one need not be sent again.
 *
 * The redraw runs at 5 Hz but the content clock is one second (SECOND_US): rssi, the history
 * sample and the relay rates only move on that tick, and a resting «Связь» screen does not move
 * for minutes. Four of every five passes were pushing the same 1024 bytes — by display_hal's
 * own arithmetic, 64 transfers a frame, 320 a second — down the one bus this task is supposed
 * to be the cheapest user of.
 *
 * REFRESH_FLOOR_US is why this is not simply "skip when equal". The panel's contents are state
 * held in the SSD1306, not in this firmware: a glitch on the wire, a brown-out on the panel's
 * rail, a controller that misses a command leaves it showing something this code has no way to
 * detect. Never resending would make that permanent. Every five seconds the frame goes out
 * whatever the comparison says, so the worst a corrupted panel can be is five seconds stale,
 * and the saving on a resting screen is still four passes in five. */
#define REFRESH_FLOOR_US (5 * 1000 * 1000LL)
static screen_t s_shown;
static uint16_t s_shown_hist;
static bool     s_shown_valid;
static int64_t  s_shown_us;

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

    /* Each segment's two edges are computed from the rule's own extent rather than from a
     * uniform width laid out and then centred. A uniform width drops the remainder of the
     * division — with four pages, (92 - 18) / 4 is 18 and the run comes to 90, a pixel short
     * of the rule at each end. Since the markers ARE the rule, ending a pixel inside it is
     * exactly the misalignment a person notices and cannot name. Proportional edges make the
     * first segment start at RULE_X and the last end at RULE_X + RULE_W, and spend the
     * remainder as a one-pixel difference in segment width instead — invisible where a
     * misaligned end is not. */
    for (int i = 0; i < pages; i++) {
        int x0 = RULE_X + (RULE_W + MARK_GAP) * i / pages;
        int x1 = RULE_X + (RULE_W + MARK_GAP) * (i + 1) / pages - MARK_GAP;
        int w = x1 - x0;
        if (w < 1) w = 1;   /* more pages than the rule has room for: still draw something */

        if (i == (int)s->page) {
            u8g2_DrawBox(&s_u8g2, x0, RULE_Y - 1, w, 3);
        } else {
            draw_dithered(x0, RULE_Y, w);
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

/* `hist_mark` fingerprints the history ring, which the strip is drawn from and screen_t does
 * not carry — without it an unchanged screen_t would hide a moved graph. Zero on every screen
 * that does not draw the strip, so a push a second does not force a redraw of a page that never
 * shows it. */
static void draw(const screen_t *s, uint16_t hist_mark, int64_t now_us)
{
    /* memcmp is honest here only because every screens_* entry point memsets its output first,
     * so no padding byte is ever left holding whatever was on the stack. */
    if (s_shown_valid && hist_mark == s_shown_hist &&
        memcmp(s, &s_shown, sizeof(*s)) == 0 &&
        (now_us - s_shown_us) < REFRESH_FLOOR_US) {
        return;
    }

    u8g2_ClearBuffer(&s_u8g2);

    u8g2_SetFont(&s_u8g2, u8g2_font_10x20_t_cyrillic);
    draw_centred(HEAD_BASE, s->head);

    draw_rule(s);

    u8g2_SetFont(&s_u8g2, u8g2_font_6x12_t_cyrillic);
    draw_centred(ROW0_BASE, s->row[0]);
    draw_centred(ROW1_BASE, s->row[1]);

    /* The one call that touches the bus. Everything above is memory. */
    u8g2_SendBuffer(&s_u8g2);

    s_shown = *s;
    s_shown_hist = hist_mark;
    s_shown_valid = true;
    s_shown_us = now_us;
}

/* --- the view ---------------------------------------------------------------------------- */

/* screens.c renders an address's first octet from bit 31 down — `(ip_msb_first >> 24) & 0xFF`
 * — and its host test pins that: 0xC0A80402 must read "192.168.4.2". The value it wants is
 * therefore the address as a NUMBER with the leading octet most significant, which on this
 * little-endian target is the byte-reverse of what lwIP keeps in esp_ip4_addr_t.addr.
 * Assembled octet by octet through esp_netif's own accessors rather than byte-swapped, so this
 * says which octet goes where instead of depending on the build's endianness to come out
 * right — and so the field's name and its contents agree without a comment holding them
 * together. */
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

    /* Not one snapshot, and the order is what decides which half of each race can reach the
     * glass. status_api.c's status_get() carries the twin of the middle paragraph; this is the
     * caller that made the other two matter.
     *
     * The attempt count BEFORE the state, because wifi_sta publishes the two as separate
     * atomic stores: the failure that spends the last attempt sets the count to the budget and
     * the state to failed, and a read that caught the new count under the old state would put
     * «Попытка 6 из 5» on the panel. Taken first, the count can only be older than the state
     * that frames it — and a count older than a «Поиск сети» was inside the budget.
     *
     * The state BEFORE the radio's figures, because wifi_sta_ap_info() is an unlocked
     * esp_wifi_sta_get_ap_info rather than a read of wifi_sta's mirror. This way round leaves
     * only the pessimistic half: a «Связь» screen over figures that went stale between the two
     * reads, never live figures under a state that had already dropped. screens.c is what
     * makes that harmless — it spells the 0 «нет» and draws no gauge from it.
     *
     * Both figures from ONE query, so they cannot contradict each other on the way to a row
     * that shows them side by side. */
    v->attempts = wifi_sta_attempts();
    v->attempts_max = WIFI_JOIN_ATTEMPTS;
    v->state = wifi_sta_state_name();
    wifi_sta_ap_info(&v->rssi, &v->channel);

    v->host_attached = usb_net_host_attached();
    /* An unsynchronised copy of a file-static the httpd task rewrites — deliberately, and
     * net_api.h's own comment is where the reasoning lives rather than duplicated here. */
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
     * beside a zeroed address would read as a link that half exists.
     *
     * s_sta_netif, and NOT esp_netif_get_handle_from_ifkey() on every pass. That lookup is not
     * the local table walk its name suggests: esp_netif_lwip.c:909 routes it through
     * esp_netif_lwip_ipc_call_get_netif, and with CONFIG_LWIP_TCPIP_CORE_LOCKING unset in this
     * build (it is absent from build/config/sdkconfig.h) esp_netif_lwip_ipc_call_msg takes the
     * process-wide api_lock_sem with a timeout of ZERO — sys_arch_sem_wait's spelling of "wait
     * forever" — and then blocks until the tcpip thread services the message. That is the
     * thread carrying every relayed packet, and this task would have joined the queue for it
     * five times a second: an unbounded wait on a global lock, in the name of avoiding
     * wifi_sta's bounded local one. Resolved once in display_start() instead — wifi_sta_start()
     * creates the station netif and nothing ever destroys it, so the handle cannot go stale.
     *
     * esp_netif_get_ip_info() below is a different matter and stays where it is: a direct read
     * of the lwip_netif struct, no IPC at all (esp_netif_lwip.c:1980-1996). */
    esp_netif_ip_info_t ip;
    if (s_sta_netif != NULL && esp_netif_get_ip_info(s_sta_netif, &ip) == ESP_OK) {
        v->ip_msb_first = view_addr(&ip.ip);
        v->gw_msb_first = view_addr(&ip.gw);
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
    /* The age of the fault, not the stamp: screens.c has no clock, and this task already holds
     * the one relay_stats is kept on — relay_stats.h puts every ms field in that struct on
     * esp_timer milliseconds precisely so this subtraction is legitimate. Unsigned throughout,
     * so it is correct across the wrap. Read after last_errno: a stamp older than the errno it
     * is paired with overstates the age, which errs towards "this fault is stale" — the
     * direction that makes a reader look harder rather than relax. */
    v->fault_age_s = (uint32_t)(((uint32_t)(now_us / 1000) - r->last_fail_ms) / 1000u);

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
    if (!s_button_ok) return;   /* the pin was never made an input; see s_button_ok */

    bool down = gpio_get_level(BOARD_BOOT_GPIO) == 0;   /* pulled up; pressed is low */

    if (down && !s_button_down) {
        s_page = screens_next_page(s_page);   /* the cycle itself is screens.c's, and tested there */
        s_press_us = now_us;
    }
    s_button_down = down;

    /* Back to the state screen five seconds after the last press. The reference pages are
     * something a person went looking for; the state screen is what the device is for. */
    if (s_page != SCREENS_PAGE_STATE && now_us - s_press_us > PAGE_HOLD_US) {
        s_page = SCREENS_PAGE_STATE;
    }
}

/* --- the task ---------------------------------------------------------------------------- */

static void display_task(void *arg)
{
    (void)arg;

    const esp_app_desc_t *app = esp_app_get_description();
    TickType_t last_wake = xTaskGetTickCount();

    /* Seeded from the moment display_start() opened the first rate window, not from this
     * task's first pass. That is what keeps the first second from closing early — a window a
     * few milliseconds wide, divided into whatever the relays had forwarded — without a
     * special case for it. */
    int64_t last_second_us = s_start_us;

    for (;;) {
        int64_t now_us = esp_timer_get_time();
        bool second = (now_us - last_second_us) >= SECOND_US;
        if (second) last_second_us = now_us;

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
        } else if (s_page == SCREENS_PAGE_STATE) {
            screens_for(&v, &s);
        } else if (s_page < (int)screens_diag_pages()) {
            screens_diag(&v, (uint8_t)s_page, &s);
        } else {
            screens_signal(&v, &s);
        }

        /* count and next together, not either alone: next wraps at SCREEN_HISTORY, so a full
         * ring returning to the same slot would fingerprint identically on its own. */
        uint16_t hist_mark = 0;
        if (s.gauge == GAUGE_HISTORY) {
            hist_mark = (uint16_t)(((uint16_t)s_history.count << 8) | s_history.next);
        }
        draw(&s, hist_mark, now_us);

        /* vTaskDelayUntil, so a slow pass — an I2C bus holding the line, say — costs cadence
         * and not drift. What it cannot do is give back the time a slow pass already spent,
         * which is why the second above is measured rather than counted. */
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(PERIOD_MS));
    }
}

esp_err_t display_start(void)
{
    screens_history_init(&s_history);
    s_start_us = esp_timer_get_time();
    s_press_us = s_start_us;

    /* Once, here, and never again from the task — see view_build(). Not fatal if it comes back
     * NULL: only the address page loses its two rows. */
    s_sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (s_sta_netif == NULL) {
        ESP_LOGE(TAG, "no station netif to read an address from — «Диагностика» page 1 will "
                      "show 0.0.0.0");
    }

    /* Opens the first rate window at NOW rather than at zero. relay_stats_init leaves mark_ms
     * at 0, so without this the first sample would divide the packets forwarded since boot by
     * the whole uptime and publish that as a current rate — an average dressed as a reading.
     * The rate this latches is honest: the relays wait on a gateway and the station has not
     * joined at this point in app_main, so nothing has been forwarded yet. */
    relay_stats_sample(relay_stats_shared(), (uint32_t)(s_start_us / 1000));

    gpio_config_t btn = {
        .pin_bit_mask = 1ULL << BOARD_BOOT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&btn);
    s_button_ok = (err == ESP_OK);
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
