#ifndef SCREENS_H
#define SCREENS_H

#include <stdbool.h>
#include <stdint.h>

#include "dongle_contract.inc"

/* Every string a person will ever read on this device's panel, and the logic that picks which
 * one is showing.
 *
 * Pure: no ESP-IDF, no clock, no I2C, so both are host-tested with plain `cc` rather than
 * reasoned about on a bench with a soldered panel. display.c owns the u8g2 calls and the 5 Hz
 * task that samples the clock and the radio; it holds no copy and no layout of its own. This
 * is also the ONLY file the project's Global Constraints on wording bind — every other file
 * that happens to log a message is not addressing the person holding the dongle.
 *
 * Two widths this panel is stuck with, because they are the fonts' pixel budgets rather than
 * a preference: SCREEN_HEAD_GLYPHS in u8g2_font_10x20_t_cyrillic, SCREEN_ROW_GLYPHS in
 * u8g2_font_6x12_t_cyrillic. Russian is two bytes per character in UTF-8, so a glyph count and
 * a byte count are two different numbers for every line with a single Cyrillic letter in it —
 * strlen() would answer the wrong question. */

/* One sample per second. 46 of them fill the rule's own 92-pixel width at two pixels each, which
 * is why the number is 46 and not a round 60: the screen decides how much history there is
 * room for, and inventing samples the panel cannot draw would be a graph that lies about its
 * own resolution. */
#define SCREEN_HISTORY 46

/* Two rows beneath the headline and the rule, same as the panel has room for. */
#define SCREEN_ROWS 2

/* The panel's own budgets, in GLYPHS: the fonts are fixed-width, so 128 px holds twelve of the
 * 10x20 headline's characters and twenty-one of the 6x12 rows'. This is the only limit the
 * glass enforces — anything past it is drawn off the edge and clipped, with no error and no
 * mark on the screen to say so. Named here rather than left as literals in the test that
 * checks them, because screens.c has to cut the two rows it does not author (an SSID and a
 * version string) against exactly this number, and a budget only the test knows is a budget
 * the code cannot keep. */
#define SCREEN_HEAD_GLYPHS 12
#define SCREEN_ROW_GLYPHS  21

/* The buffers, in BYTES, derived from those budgets rather than stated beside them — the two
 * are not the same number. Cyrillic spends two UTF-8 bytes per glyph, so a row that runs
 * all-Cyrillic to the edge needs 21*2 = 42 bytes of payload plus a NUL: 43; the headline, 25.
 * Sized from the panel's real limit rather than from any one line of copy, so a future line
 * that happens to run all-Cyrillic to the edge of the row still fits. (A narrower buffer sized
 * only off the shorter mixed-script rows in the table below would silently truncate the
 * all-Cyrillic ones — "Задаёт приложение" alone is 33 bytes, already past a 22-byte guess.) */
#define SCREEN_ROW_MAX  (2 * SCREEN_ROW_GLYPHS + 1)
#define SCREEN_HEAD_MAX (2 * SCREEN_HEAD_GLYPHS + 1)

typedef enum {
    SCREEN_SPLASH = 0,     /* the two seconds after boot, and the fallback for a state this
                               build does not recognise — introduce the device rather than
                               guess at a network it may not even have queried yet */
    SCREEN_UNCONFIGURED,   /* DONGLE_STATE_IDLE: never told a network to join */
    SCREEN_UPDATING,       /* an OTA upload is in flight */
    SCREEN_SEARCHING,      /* DONGLE_STATE_SEARCHING */
    SCREEN_JOINING,        /* DONGLE_STATE_JOINING */
    SCREEN_LINKED,         /* DONGLE_STATE_CONNECTED */
    SCREEN_NO_NETWORK,     /* DONGLE_STATE_FAILED: the join budget ran out */
    SCREEN_ROLLED_BACK,    /* the bootloader reverted the previous OTA */
    SCREEN_NO_HOST,        /* no USB host attached — nothing to say about a network either */
    SCREEN_DIAG,           /* reached only by pressing BOOT — five pages, the signal first */
    SCREEN_RESET,          /* BOOT held past the short-press window: the erase countdown */
} screen_id_t;

typedef enum {
    GAUGE_NONE = 0,   /* the rule is undecorated */
    GAUGE_LEVEL,      /* the rule fills like a battery gauge, to gauge_pct */
    GAUGE_HISTORY,    /* the rule draws the RSSI history instead of filling */
} screen_gauge_t;

typedef struct {
    screen_id_t     id;
    char            head[SCREEN_HEAD_MAX];
    char            row[SCREEN_ROWS][SCREEN_ROW_MAX];
    screen_gauge_t  gauge;
    uint8_t         gauge_pct;  /* meaningful only when gauge == GAUGE_LEVEL, 0..100 */
    uint8_t         pages;      /* meaningful only for SCREEN_DIAG: how many pages exist */
    uint8_t         page;       /* meaningful only for SCREEN_DIAG: which one this is, 0-based */
} screen_t;

/* One sample of history: a ring of the RSSI readings the display itself has taken, oldest
 * falling off as new ones push in. Arithmetic, not I/O, which is why it lives here rather
 * than in display.c — display.c owns only the one-second clock that feeds it. */
typedef struct {
    int8_t  dbm[SCREEN_HISTORY];
    uint8_t count;   /* how many are valid, saturating at SCREEN_HISTORY */
    uint8_t next;    /* where the next push lands */
} screens_history_t;

void    screens_history_init(screens_history_t *h);
void    screens_history_push(screens_history_t *h, int8_t dbm);
/* Oldest first into out[SCREEN_HISTORY]; returns how many were written. */
uint8_t screens_history_read(const screens_history_t *h, int8_t *out);
/* The weakest sample recorded, or 0 when nothing has been. */
int8_t  screens_history_min(const screens_history_t *h);

/* Everything the display needs to pick and draw a screen — and nothing else. Only the
 * dongle's OWN measurements: its radio, its USB link, its OTA state. No car telemetry ever
 * belongs here, and no relayed-packet rate labelled in hertz — relay_stats.h's counters are
 * this dongle's own observation of what it moved, not a claim about the car's control loop
 * (see relay_stats.h for why mixing the two would be dishonest). */
typedef struct {
    const char *state;        /* one of DONGLE_STATE_*; anything else falls back to the splash */
    bool        host_attached;

    const char *ssid;         /* the configured (or joining, or joined) network */
    int8_t      rssi;         /* dBm, this dongle's own receiver */
    uint8_t     channel;
    uint8_t     attempts;
    uint8_t     attempts_max;

    const char *fw;           /* this dongle's own firmware version */
    bool        rolled_back;

    bool        ota_active;
    uint32_t    ota_done;     /* bytes */
    uint32_t    ota_total;    /* bytes */

    /* The leading octet is the MOST SIGNIFICANT byte, so 192.168.4.2 is 0xC0A80402 — screens.c
     * renders from bit 31 down and test_screens pins exactly that. On a little-endian target
     * this is the byte-REVERSE of what lwIP keeps in esp_ip4_addr_t.addr, so a raw address
     * assigned straight in would render as 2.4.168.192; display.c's view_addr() assembles the
     * octets instead. These were called ip_be/gw_be, which said the opposite of what they hold
     * and left the next caller one silent assignment away from that — a comment cannot stop a
     * field whose name invites the mistake. Both of them carry the WIFI STATION's numbers, not
     * the USB side's. */
    uint32_t    ip_msb_first;   /* this dongle's own address on the network it joined */
    uint32_t    gw_msb_first;   /* that network's gateway */

    int         last_errno;   /* 0 when nothing has failed; relay_stats_t's own type */
    uint32_t    errno_count;
    /* How long ago the last failure was, in seconds. Meaningless when last_errno is 0, and not
     * read then. It is what separates a link failing right now from one transient errno at boot
     * three hours ago — the two left this view identical, and the fault page reported both as
     * current. relay_stats.c keeps the stamp; the age is the caller's subtraction because this
     * module has no clock (see relay_stats.h, which keeps every ms field on one). */
    uint32_t    fault_age_s;

    uint16_t    to_car_x10;   /* packets per second, x10 — relay_stats_t's own unit */
    uint16_t    to_phone_x10;
    uint8_t     udp_used;
    uint8_t     tcp_used;

    uint32_t    uptime_s;

    const screens_history_t *history;  /* owned by display.c; may be NULL */
} dongle_view_t;

/* The precedence, highest first: no USB host, then an OTA in flight, then a rolled-back
 * image, then the station's own state. A dongle with no host attached has nothing to say
 * about a network — there is no phone there to read it — so that check runs first and
 * unconditionally, ahead of even an update or a rollback. */
void screens_for(const dongle_view_t *v, screen_t *out);

/* Reached only by paging with BOOT. `page` wraps the caller's responsibility, not this
 * function's — pass anything in range and it renders exactly that page. Page 0 is the signal:
 * gauge == GAUGE_HISTORY and a single row, which display.c lays out under the strip; the
 * other pages have two rows and no gauge. */
void    screens_diag(const dongle_view_t *v, uint8_t page, screen_t *out);
uint8_t screens_diag_pages(void);

/* Shown while BOOT is held past the short-press window and until the erase fires. `pct` is
 * how far along the hold is — the level gauge is the countdown — and clamps at 100. Pure:
 * what the hold does and when it fires belong to display.c, which owns the button. */
void screens_reset(uint8_t pct, screen_t *out);

/* Where the BOOT button has paged to. SCREENS_PAGE_STATE means "showing the state screen",
 * which is both the resting place and where the five-second timeout returns to; 0..diag_pages-1
 * are the reference pages, the signal first. */
#define SCREENS_PAGE_STATE (-1)

/* The page one press of BOOT moves to from `page`. Here and not in the task that polls the pin:
 * it is integer logic about this module's own pages, and in display.c no host test could reach
 * it — which is how the wrap came to be asserted by a test that only ever counted the pages. */
int screens_next_page(int page);

/* Maps this radio's working range to a fill percentage: -85 dBm is where a join stops
 * holding, -25 dBm is desk distance. Clamped at both ends. */
uint8_t screens_rssi_pct(int8_t dbm);

#endif /* SCREENS_H */
