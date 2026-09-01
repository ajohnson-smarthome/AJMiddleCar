#ifndef DISPLAY_H
#define DISPLAY_H

#include "esp_err.h"

/* The panel's task: five times a second it reads what this dongle knows about itself, asks
 * screens.c which of the eleven screens that is, and draws it.
 *
 * It holds no state anybody else can see and offers nothing to call — the screen is an
 * instrument, and an instrument is read, not queried. The one thing it owns that outlives a
 * pass is the RSSI history behind the «Сигнал» page: the ring is arithmetic and lives in
 * screens.c, but the once-a-second clock that fills it belongs here.
 *
 * Start it LAST in app_main, after the server and its handlers and before the rollback
 * waiver — see the comment at the call site for why that ordering is a safety property and
 * not a preference.
 *
 * Returns ESP_OK when the task is running, which is all this can honestly claim: whether the
 * panel is wired, addressed and lit is a runtime condition, reported by display_hal's log and
 * by nothing else. It is also worth knowing that this task is the only caller of
 * relay_stats_sample(), so GET /status's packet rates go with it. */
esp_err_t display_start(void);

#endif /* DISPLAY_H */
