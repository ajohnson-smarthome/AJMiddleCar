#include <stdio.h>
#include <string.h>

#include "screens.h"

static int failures;
static void check(int ok, const char *what)
{
    if (!ok) { printf("FAIL: %s\n", what); failures++; }
}

/* The font's limit is 12 GLYPHS, and Cyrillic is two bytes each in UTF-8, so strlen() would
 * measure the wrong thing and reject correct copy. Counting lead bytes counts characters. */
static size_t glyphs(const char *s)
{
    size_t n = 0;
    for (; *s; s++) if ((*s & 0xC0) != 0x80) n++;
    return n;
}

static void check_fits(const screen_t *s)
{
    check(glyphs(s->head) <= 12, "headline fits the 10x20 font");
    for (int r = 0; r < SCREEN_ROWS; r++)
        check(glyphs(s->row[r]) <= 21, "row fits the 6x12 font");
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

static void test_searching_shows_the_attempt_budget(void)
{
    dongle_view_t v = base();
    v.state = DONGLE_STATE_SEARCHING;
    v.attempts = 3;
    screen_t s;
    screens_for(&v, &s);
    check(s.id == SCREEN_SEARCHING, "searching");
    check(strstr(s.row[1], "3") && strstr(s.row[1], "5"), "the row carries 3 of 5");
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

int main(void)
{
    test_every_headline_fits_twelve_characters();
    test_no_host_wins_over_every_network_state();
    test_rollback_outranks_the_network();
    test_update_outranks_rollback();
    test_searching_shows_the_attempt_budget();
    test_linked_fills_the_rule_from_the_signal();
    test_rssi_maps_over_the_range_that_matters();
    test_diagnostics_pages_are_four_and_wrap();
    test_a_quiet_relay_reports_no_error();
    test_a_faulted_relay_names_the_errno_and_the_repeats();
    test_history_reads_back_oldest_first();
    test_history_wraps_and_drops_the_oldest();
    test_an_empty_history_has_no_minimum_to_report();
    printf(failures ? "screens: %d FAILED\n" : "screens: ok\n", failures);
    return failures != 0;
}
