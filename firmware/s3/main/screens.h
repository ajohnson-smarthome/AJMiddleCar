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
 * a preference: a headline is at most 12 GLYPHS (u8g2_font_10x20_t_cyrillic), a row at most 21
 * GLYPHS (u8g2_font_6x12_t_cyrillic). Russian is two bytes per character in UTF-8, so a glyph
 * count and a byte count are two different numbers for every line with a single Cyrillic
 * letter in it — strlen() would answer the wrong question. */

/* One sample per second. 46 of them fill the rule's own 92-pixel width at two pixels each, which
 * is why the number is 46 and not a round 60: the screen decides how much history there is
 * room for, and inventing samples the panel cannot draw would be a graph that lies about its
 * own resolution. */
#define SCREEN_HISTORY 46

/* Two rows beneath the headline and the rule, same as the panel has room for. */
#define SCREEN_ROWS 2

/* Bytes, not glyphs — and the two are not the same number here. A row's glyph budget is 21,
 * and Cyrillic spends two UTF-8 bytes per glyph, so a row that is entirely Cyrillic needs
 * 21*2 = 42 bytes of payload plus a NUL: 43. Sized from the panel's real limit rather than
 * from any one line of copy, so a future line that happens to run all-Cyrillic to the edge of
 * the row still fits. (A narrower buffer sized only off the shorter mixed-script rows in the
 * table below would silently truncate the all-Cyrillic ones — "Задаёт приложение" alone is 33
 * bytes, already past a 22-byte guess.) */
#define SCREEN_ROW_MAX 43

/* Bytes for the headline: 12 Cyrillic glyphs are up to 24 UTF-8 bytes, plus a NUL — 25. The
 * panel's limit is glyphs, the buffer's is bytes, and they are not the same number. */
#define SCREEN_HEAD_MAX 25

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
    SCREEN_SIGNAL,         /* reached only by paging past the diagnostics with BOOT */
    SCREEN_DIAG,           /* reached only by pressing BOOT — four pages */
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

    uint32_t    ip_be;        /* this dongle's own address, network byte order */
    uint32_t    gw_be;        /* the phone acting as gateway, network byte order */

    int         last_errno;   /* 0 when nothing has failed; relay_stats_t's own type */
    uint32_t    errno_count;

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

/* Reached only by paging with BOOT, past the diagnostics pages. */
void screens_signal(const dongle_view_t *v, screen_t *out);

/* Reached only by paging with BOOT. `page` wraps the caller's responsibility, not this
 * function's — pass anything in range and it renders exactly that page. */
void    screens_diag(const dongle_view_t *v, uint8_t page, screen_t *out);
uint8_t screens_diag_pages(void);

/* Maps this radio's working range to a fill percentage: -85 dBm is where a join stops
 * holding, -25 dBm is desk distance. Clamped at both ends. */
uint8_t screens_rssi_pct(int8_t dbm);

#endif /* SCREENS_H */
