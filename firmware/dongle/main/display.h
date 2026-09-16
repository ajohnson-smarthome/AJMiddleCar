#ifndef DISPLAY_H
#define DISPLAY_H

#include "esp_err.h"

/* The panel's task: five times a second it reads what this dongle knows about itself, asks
 * screens.c which of the twelve screens that is, and draws it.
 *
 * It holds no state anybody else can see and offers nothing to call — the screen is an
 * instrument, and an instrument is read, not queried. The one thing it owns that outlives a
 * pass is the RSSI history behind the «Сигнал» page: the ring is arithmetic and lives in
 * screens.c, but the once-a-second clock that fills it belongs here.
 *
 * Two entry points, at the two ends of app_main. display_early() goes FIRST: it brings the
 * panel up and puts the splash on it before anything that can fail loudly, so the glass is
 * not dark (or, after an OTA restart, not frozen on the previous image's last frame) for the
 * half-second to second and a half that USB, Wi-Fi, the relays and the server take. It needs
 * nothing that is not there at the first millisecond — the version string is in the app
 * descriptor — and it is bounded: one probe of the panel's address, and no init at all if
 * nothing answers. display_start() goes LAST, after the server and its handlers and before
 * the rollback waiver — see the comment at the call site for why that ordering is a safety
 * property and not a preference — and starts the task on the panel display_early() left lit.
 * Neither returns an error for a panel that is not there: the panel is the least load-bearing
 * thing on the board, and a screen that will not start is logged and lived with, never a
 * reason to hold up the boot or revert a firmware that works.
 *
 * Returns ESP_OK when the task is running, which is all this can honestly claim: whether the
 * panel is wired, addressed and lit is a runtime condition, reported by display_hal's log and
 * by nothing else. It is also worth knowing that this task is the only caller of
 * relay_stats_sample(), so GET /status's packet rates go with it. */
void      display_early(void);
esp_err_t display_start(void);

/* Call before a deliberate esp_restart(): puts «Перезапуск» on the glass and returns once it is
 * there (bounded at two of the task's periods), so the previous frame — «Обновление» at 100 %,
 * say — does not sit through the reboot looking like the present. The SSD1306 keeps its RAM
 * across the MCU's reset; this is the only way the glass gets to say that something happened.
 * From any task but the display's own, which draws the frame itself where it fires the erase.
 * Once set, nothing else is drawn again in this boot. */
void      display_reboot(void);

#endif /* DISPLAY_H */
