#include <stdio.h>
#include <string.h>

#include "screens.h"

static int failures;
static void check(int ok, const char *what)
{
    if (!ok) { printf("FAIL: %s\n", what); failures++; }
}

/* The fonts' limits are in GLYPHS, and Cyrillic is two bytes each in UTF-8, so strlen() would
 * measure the wrong thing and reject correct copy. Counting lead bytes counts characters. */
static size_t glyphs(const char *s)
{
    size_t n = 0;
    for (; *s; s++) if ((*s & 0xC0) != 0x80) n++;
    return n;
}

/* Well-formed UTF-8: every lead byte followed by exactly the continuation bytes its own shape
 * announces. This is the property a byte-count truncation destroys — it cuts a two-byte
 * Cyrillic letter in half and leaves a lead byte with nothing after it, which is not a
 * character at all. */
static int valid_utf8(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    while (*p != '\0') {
        int extra;
        if (*p < 0x80) extra = 0;
        else if ((*p & 0xE0) == 0xC0) extra = 1;
        else if ((*p & 0xF0) == 0xE0) extra = 2;
        else if ((*p & 0xF8) == 0xF0) extra = 3;
        else return 0;
        p++;
        for (int i = 0; i < extra; i++, p++)
            if ((*p & 0xC0) != 0x80) return 0;
    }
    return 1;
}

static void check_fits(const screen_t *s)
{
    check(glyphs(s->head) <= SCREEN_HEAD_GLYPHS, "headline fits the 10x20 font");
    for (int r = 0; r < SCREEN_ROWS; r++) {
        check(glyphs(s->row[r]) <= SCREEN_ROW_GLYPHS, "row fits the 6x12 font");
        check(valid_utf8(s->row[r]), "row is a whole string of whole characters");
    }
}

static dongle_view_t base(void)
{
    dongle_view_t v;
    memset(&v, 0, sizeof(v));
    v.state = DONGLE_STATE_CONNECTED;
    v.host_attached = true;
    v.ssid = "AJMiddleCar";
    v.rssi = -53;
    v.channel = 1;
    v.attempts = 0;
    v.attempts_max = 5;
    v.fw = "v1.0+749";
    return v;
}

/* Ruling: the five network states alone leave SCREEN_SPLASH, SCREEN_SIGNAL, SCREEN_NO_HOST,
 * SCREEN_ROLLED_BACK, SCREEN_UPDATING and SCREEN_DIAG untouched by any glyph assertion — a
 * screen exempted from this check is a screen that will overflow on real glass, where nobody
 * will be watching a test. This extends the check to all eleven, driving screens_for for the
 * splash (an unrecognised state falls back to it) and calling screens_signal directly, since
 * neither is reachable through the ordinary state precedence a base() view walks. */
static void test_every_headline_fits_twelve_characters(void)
{
    const char *states[] = { DONGLE_STATE_IDLE, DONGLE_STATE_SEARCHING, DONGLE_STATE_JOINING,
                             DONGLE_STATE_CONNECTED, DONGLE_STATE_FAILED };
    for (size_t i = 0; i < sizeof(states) / sizeof(*states); i++) {
        dongle_view_t v = base();
        v.state = states[i];
        screen_t s;
        screens_for(&v, &s);
        check_fits(&s);
    }

    /* SCREEN_NO_HOST */
    {
        dongle_view_t v = base();
        v.host_attached = false;
        screen_t s;
        screens_for(&v, &s);
        check_fits(&s);
    }

    /* SCREEN_ROLLED_BACK */
    {
        dongle_view_t v = base();
        v.rolled_back = true;
        screen_t s;
        screens_for(&v, &s);
        check_fits(&s);
    }

    /* SCREEN_UPDATING */
    {
        dongle_view_t v = base();
        v.ota_active = true;
        v.ota_done = 1140000; v.ota_total = 1830000;
        screen_t s;
        screens_for(&v, &s);
        check_fits(&s);
    }

    /* SCREEN_SPLASH — no DONGLE_STATE_* matches, so screens_for falls back to introducing
     * the device rather than guessing at a network. */
    {
        dongle_view_t v = base();
        v.state = "";
        screen_t s;
        screens_for(&v, &s);
        check(s.id == SCREEN_SPLASH, "an unrecognised state falls back to the splash");
        check_fits(&s);
    }

    /* SCREEN_SIGNAL — reached only by paging with BOOT, never through screens_for. */
    {
        dongle_view_t v = base();
        screens_history_t h;
        screens_history_init(&h);
        screens_history_push(&h, -60);
        v.history = &h;
        screen_t s;
        screens_signal(&v, &s);
        check(s.id == SCREEN_SIGNAL, "signal");
        check_fits(&s);
    }

    /* SCREEN_DIAG — all four pages, at the nominal packet rate rather than the zero the
     * base fixture leaves it at -- a page checked only at zero never renders the row a
     * person actually sees. */
    {
        dongle_view_t v = base();
        v.ip_be = 0xC0A80402; v.gw_be = 0xC0A80401;
        v.to_car_x10 = 100;
        v.to_phone_x10 = 50;
        for (uint8_t p = 0; p < screens_diag_pages(); p++) {
            screen_t s;
            screens_diag(&v, p, &s);
            check_fits(&s);
        }
    }
}

static void test_no_host_wins_over_every_network_state(void)
{
    dongle_view_t v = base();
    v.host_attached = false;
    screen_t s;
    screens_for(&v, &s);
    check(s.id == SCREEN_NO_HOST, "no USB host outranks a perfectly good join");
}

static void test_rollback_outranks_the_network(void)
{
    dongle_view_t v = base();
    v.rolled_back = true;
    screen_t s;
    screens_for(&v, &s);
    check(s.id == SCREEN_ROLLED_BACK, "a reverted image is reported over a working link");
}

static void test_update_outranks_rollback(void)
{
    dongle_view_t v = base();
    v.rolled_back = true;
    v.ota_active = true;
    v.ota_done = 1140000; v.ota_total = 1830000;
    screen_t s;
    screens_for(&v, &s);
    check(s.id == SCREEN_UPDATING, "an upload in progress is the most urgent thing on screen");
    check(s.gauge == GAUGE_LEVEL && s.gauge_pct == 62, "the rule fills to the transfer");
}

/* dongle_view_t.attempts counts the attempts CONSUMED (wifi_state.h), and this row is an
 * ordinal — so the two ends of what it can ever show are a join in flight with nothing spent
 * yet, and the last attempt of the budget. They must read 1 and 5. The whole row, not a
 * substring: the assertion this replaces set attempts to 3 and checked that the row contained
 * "3" and "5", which is true of «Попытка 3 из 5» and equally true of the off-by-one that made
 * the panel open at «Попытка 0 из 5» and never once reach 5. */
static void test_searching_counts_attempts_from_one(void)
{
    dongle_view_t v = base();
    v.state = DONGLE_STATE_SEARCHING;
    screen_t s;

    v.attempts = 0;   /* wifi_state.c's own entry value for a fresh configuration */
    screens_for(&v, &s);
    check(s.id == SCREEN_SEARCHING, "searching");
    check(strcmp(s.row[1], "Попытка 1 из 5") == 0, "a join in flight is the first attempt");

    /* 4 consumed is the fifth attempt running: the failure that takes the count to 5 is the
     * one that moves the state to «Нет сети», so this is the highest this row can reach. */
    v.attempts = 4;
    screens_for(&v, &s);
    check(strcmp(s.row[1], "Попытка 5 из 5") == 0, "the last attempt reads as the last one");
}

/* The SSID is the only text on this panel the device does not write itself, and
 * contract/dongle-api.json caps it at 32 BYTES — which is 32 Latin characters, half again the
 * row's budget of 21, and 16 Cyrillic ones, which fit whole. Both are at the contract's limit;
 * only one is past the panel's. */
static void test_an_ssid_at_the_contract_limit_stays_inside_the_row(void)
{
    const char *latin = "ABCDEFGHIJKLMNOPQRSTUVWXYZ012345";   /* 32 bytes, 32 glyphs */
    const char *cyr   = "АБВГДЕЖЗИКЛМНОПР";                   /* 32 bytes, 16 glyphs */
    const char *states[] = { DONGLE_STATE_SEARCHING, DONGLE_STATE_CONNECTED };

    for (size_t i = 0; i < sizeof(states) / sizeof(*states); i++) {
        dongle_view_t v = base();
        v.state = states[i];
        screen_t s;

        v.ssid = latin;
        screens_for(&v, &s);
        check(glyphs(s.row[0]) <= SCREEN_ROW_GLYPHS, "a 32-character SSID is cut to the row");
        check(valid_utf8(s.row[0]), "and is still a string");
        check(strstr(s.row[0], "...") != NULL, "and says that it was cut");

        v.ssid = cyr;
        screens_for(&v, &s);
        check(strcmp(s.row[0], cyr) == 0, "16 Cyrillic letters fit whole and are left alone");
        check(glyphs(s.row[0]) <= SCREEN_ROW_GLYPHS, "well inside the row");
    }
}

/* Past anything an SSID can be — 43 bytes, where the contract allows 32 — and here for the
 * property no SSID can currently exercise: the cut lands BETWEEN characters. One Latin letter
 * then 21 Cyrillic ones is 22 glyphs in 43 bytes, so a plain snprintf into SCREEN_ROW_MAX
 * would copy 42 of them and leave the last letter as a lone lead byte. */
static void test_a_row_is_never_cut_through_a_character(void)
{
    dongle_view_t v = base();
    v.ssid = "AБВГДЕЖЗИКЛМНОПРСТУФХЦ";
    screen_t s;
    screens_for(&v, &s);
    check(glyphs(s.row[0]) <= SCREEN_ROW_GLYPHS, "cut to the row's budget");
    check(valid_utf8(s.row[0]), "cut between characters, never through one");
    check(strlen(s.row[0]) < SCREEN_ROW_MAX, "and inside the buffer it was cut for");
}

static void test_linked_fills_the_rule_from_the_signal(void)
{
    dongle_view_t v = base();
    screen_t s;
    screens_for(&v, &s);
    check(s.id == SCREEN_LINKED, "connected");
    check(s.gauge == GAUGE_LEVEL, "the rule shows the level");
    check(s.gauge_pct == screens_rssi_pct(-53), "filled from RSSI");
    check(strcmp(s.row[0], "AJMiddleCar") == 0, "the network by name");
}

/* 0 dBm and channel 0 are wifi_sta's not-connected sentinels, and «Сигнал» and the radio
 * diagnostics page are exactly what a person opens during a dropout. Printed as numbers they
 * are not merely odd: 0 dBm is above the top of screens_rssi_pct's range, so it reads as — and
 * fills a gauge to — the strongest link this panel can express, at the moment there is none. */
static void test_a_dropped_link_reports_no_reading_rather_than_a_perfect_one(void)
{
    dongle_view_t v = base();
    v.rssi = 0;
    v.channel = 0;
    screen_t s;

    screens_signal(&v, &s);
    check(strstr(s.row[0], "dBm") == NULL, "«Сигнал» prints no level it does not have");
    check(strstr(s.row[0], "нет") != NULL, "it says there is none");

    screens_diag(&v, 1, &s);
    check(strcmp(s.row[0], "Канал          нет") == 0, "no channel to report");
    check(strcmp(s.row[1], "Уровень        нет") == 0, "no level to report");
    check(glyphs(s.row[0]) <= SCREEN_ROW_GLYPHS && glyphs(s.row[1]) <= SCREEN_ROW_GLYPHS,
          "both rows still fit");

    /* «Связь» is chosen from a state read before the radio's figures, so it can outlive them
     * by a pass — the one screen where the sentinel meets a gauge. */
    screens_for(&v, &s);
    check(s.id == SCREEN_LINKED, "connected, from a state read a moment earlier");
    check(s.gauge == GAUGE_NONE, "nothing is gauged from a reading that does not exist");
    check(strstr(s.row[1], "нет") != NULL, "and the row says so rather than showing 0");
}

/* The same rule for the history's own floor: screens_history_min answers 0 for a ring nothing
 * has been pushed into, which on a device half a minute out of a reboot would report its worst
 * signal as the best one it can hold. */
static void test_an_unfilled_history_reports_no_minimum(void)
{
    dongle_view_t v = base();
    screens_history_t h;
    screens_history_init(&h);
    v.history = &h;
    screen_t s;
    screens_signal(&v, &s);
    check(strstr(s.row[0], "мин нет") != NULL, "no worst yet, rather than a worst of zero");

    screens_history_push(&h, -71);
    screens_signal(&v, &s);
    check(strstr(s.row[0], "мин -71") != NULL, "and the real one once there is one");
}

static void test_rssi_maps_over_the_range_that_matters(void)
{
    check(screens_rssi_pct(-25) == 100, "desk distance is full");
    check(screens_rssi_pct(-85) == 0, "the edge of holding a join is empty");
    check(screens_rssi_pct(-55) == 50, "the middle is half");
    check(screens_rssi_pct(-10) == 100, "stronger than the range clamps");
    check(screens_rssi_pct(-120) == 0, "weaker than the range clamps");
}

static void test_diagnostics_pages_are_four_and_wrap(void)
{
    dongle_view_t v = base();
    v.ip_be = 0xC0A80402; v.gw_be = 0xC0A80401;   /* 192.168.4.2, 192.168.4.1 */
    v.last_errno = 0;
    /* The base fixture leaves these at zero, which renders a far shorter packet row than the
     * one a person actually sees -- 10.0/5.0 pkt/s is the car's nominal 10 Hz control cadence,
     * so that is the row the glyph limit below must hold at, not an empty one. */
    v.to_car_x10 = 100;
    v.to_phone_x10 = 50;
    check(screens_diag_pages() == 4, "four pages");
    for (uint8_t p = 0; p < 4; p++) {
        dongle_view_t d = v;
        d.state = DONGLE_STATE_CONNECTED;
        screen_t s;
        screens_diag(&d, p, &s);
        check(s.id == SCREEN_DIAG, "diagnostics");
        check(strcmp(s.head, "Диагностика") == 0, "the same headline on every page");
        check(s.pages == 4 && s.page == p, "the markers say which page");
        check(glyphs(s.row[0]) <= 21 && glyphs(s.row[1]) <= 21, "rows fit");
    }

    /* The clamp: to_car_x10/to_phone_x10 are each a uint16_t and can reach 6553, far past the
     * roughly 400 (40.0 pkt/s) four phone sessions at 10 Hz could plausibly produce. Both
     * rates here are past the 99.9 clamp, so this is the widest row the panel will ever be
     * asked to draw. */
    {
        dongle_view_t d = v;
        d.state = DONGLE_STATE_CONNECTED;
        d.to_car_x10 = 6553;
        d.to_phone_x10 = 1200;
        screen_t s;
        screens_diag(&d, 2, &s);
        check(glyphs(s.row[0]) <= 21 && glyphs(s.row[1]) <= 21, "rows fit even at the clamp");
        check(strstr(s.row[1], "99.9 / 99.9") != NULL, "both rates clamp at 99.9");
    }
}

static void test_a_quiet_relay_reports_no_error(void)
{
    dongle_view_t v = base();
    v.last_errno = 0;
    screen_t s;
    screens_diag(&v, 3, &s);
    check(strstr(s.row[0], "нет") != NULL, "no fault reads as none, not as zero");
}

static void test_history_reads_back_oldest_first(void)
{
    screens_history_t h;
    screens_history_init(&h);
    screens_history_push(&h, -50);
    screens_history_push(&h, -60);
    screens_history_push(&h, -55);
    int8_t out[SCREEN_HISTORY];
    check(screens_history_read(&h, out) == 3, "three samples");
    check(out[0] == -50 && out[1] == -60 && out[2] == -55, "oldest first");
    check(screens_history_min(&h) == -60, "the worst dip, which is what breaks a link");
}

static void test_history_wraps_and_drops_the_oldest(void)
{
    screens_history_t h;
    screens_history_init(&h);
    for (int i = 0; i < SCREEN_HISTORY + 2; i++)
        screens_history_push(&h, (int8_t)(-40 - i));
    int8_t out[SCREEN_HISTORY];
    check(screens_history_read(&h, out) == SCREEN_HISTORY, "full, never more");
    check(out[0] == (int8_t)(-40 - 2), "the two oldest fell off the end");
    check(out[SCREEN_HISTORY - 1] == (int8_t)(-40 - (SCREEN_HISTORY + 1)), "newest last");
}

static void test_an_empty_history_has_no_minimum_to_report(void)
{
    screens_history_t h;
    screens_history_init(&h);
    int8_t out[SCREEN_HISTORY];
    check(screens_history_read(&h, out) == 0, "nothing recorded yet");
    check(screens_history_min(&h) == 0, "and no minimum invented for it");
}

static void test_a_faulted_relay_names_the_errno_and_the_repeats(void)
{
    dongle_view_t v = base();
    v.last_errno = 12;
    v.errno_count = 1483;
    screen_t s;
    screens_diag(&v, 3, &s);
    check(strstr(s.row[0], "12") && strstr(s.row[0], "1483"), "the fault and how often");
}

/* Every earlier test that measured the fault page left last_errno at zero, so it only ever
 * took the static "нет" branch — the dynamic one, which is the one with an unbounded field
 * in it, had never actually been rendered by a test. Drives both errno and errno_count past
 * their clamps at once, since that is the widest row diag_page_fault can ever produce. */
static void test_the_fault_row_stays_in_budget_at_its_widest(void)
{
    dongle_view_t v = base();
    v.last_errno = 12345;        /* clamps to 999 */
    v.errno_count = 999999999u;  /* clamps to 99999, marked with a trailing '+' */
    screen_t s;
    screens_diag(&v, 3, &s);
    check(glyphs(s.row[0]) <= 21, "the fault row fits even past both clamps");
    check(strstr(s.row[0], "999") != NULL, "errno clamps to three digits");
    check(strstr(s.row[0], "99999+") != NULL, "the count clamps and marks that it did");
}

/* Same shape, same reason: uptime_s is unbounded too, and every earlier test left it at zero. */
static void test_the_uptime_row_stays_in_budget_at_its_clamp(void)
{
    dongle_view_t v = base();
    v.uptime_s = 0xFFFFFFFFu;  /* far past the ~11.4-year trigger for a 6th hour digit */
    screen_t s;
    screens_diag(&v, 3, &s);
    check(glyphs(s.row[1]) <= 21, "the uptime row fits even at the clamp");
    check(strstr(s.row[1], "99999:") != NULL, "the hours field clamps");
}

int main(void)
{
    test_every_headline_fits_twelve_characters();
    test_no_host_wins_over_every_network_state();
    test_rollback_outranks_the_network();
    test_update_outranks_rollback();
    test_searching_counts_attempts_from_one();
    test_an_ssid_at_the_contract_limit_stays_inside_the_row();
    test_a_row_is_never_cut_through_a_character();
    test_linked_fills_the_rule_from_the_signal();
    test_a_dropped_link_reports_no_reading_rather_than_a_perfect_one();
    test_an_unfilled_history_reports_no_minimum();
    test_rssi_maps_over_the_range_that_matters();
    test_diagnostics_pages_are_four_and_wrap();
    test_a_quiet_relay_reports_no_error();
    test_a_faulted_relay_names_the_errno_and_the_repeats();
    test_the_fault_row_stays_in_budget_at_its_widest();
    test_the_uptime_row_stays_in_budget_at_its_clamp();
    test_history_reads_back_oldest_first();
    test_history_wraps_and_drops_the_oldest();
    test_an_empty_history_has_no_minimum_to_report();
    printf(failures ? "screens: %d FAILED\n" : "screens: ok\n", failures);
    return failures != 0;
}
