#ifndef DISPLAY_HAL_H
#define DISPLAY_HAL_H

#include "esp_err.h"

#include "u8g2.h"

/* The only glue between u8g2 and this board: u8g2's byte and gpio/delay callbacks, over the
 * IDF I2C master driver. Everything physical it needs — pins, bus speed, panel address —
 * comes from board.h and from nowhere else, so bring-up edits that file rather than this one.
 *
 * Not a layout module and not a policy module: it knows the SSD1306 is on I2C and nothing
 * about what is drawn on it. display.c owns the picture. */

/* Bring up the I2C bus, add the panel as a device on it, and hand u8g2 a fully configured
 * u8g2_t (SSD1306 128x64, U8G2_R0, full framebuffer). Call once.
 *
 * Returns the first error that stopped the bus or the device from existing. On a failure the
 * u8g2_t is still set up and every later u8g2 call is safe — the byte callback discards its
 * bytes instead of transmitting them, so a caller may keep drawing into RAM with nothing on
 * the wire. That is deliberate: display.c's task is also the only caller of
 * relay_stats_sample(), and a panel that never answers must not take GET /status's packet
 * rates down with it. */
esp_err_t display_hal_setup(u8g2_t *u8g2);

/* One bounded question to the panel: does anything acknowledge its address? At most
 * XFER_TIMEOUT_MS on a wedged bus, nothing on a healthy one. For display_early(), which runs
 * before everything else in app_main and must not spend the init sequence's dozens of
 * transfers — each with its own timeout — finding out that no panel is wired. ESP_OK when the
 * panel answered; the failure is not logged here, because the task's own transfers will say
 * so with the throttle they already have. */
esp_err_t display_hal_probe(void);

#endif /* DISPLAY_HAL_H */
