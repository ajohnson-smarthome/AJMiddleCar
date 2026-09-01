#include <string.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board.h"
#include "display_hal.h"

static const char *TAG = "display_hal";

/* NOTHING HERE HAS EVER RUN AGAINST GLASS. The panel had not been bought when this was
 * written, so every constant below is chosen from the datasheet and from u8g2's own source,
 * not confirmed on a bench. firmware/s3/README.md's table lists what the panel still owes. */

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;

/* One I2C transaction's worth of bytes, accumulated between START_TRANSFER and END_TRANSFER
 * and written in a single i2c_master_transmit.
 *
 * 32 is comfortably above the largest transfer u8g2 can hand this callback, and the ceiling
 * is u8g2's own, not a guess: u8x8_cad_ssd13xx_fast_i2c (u8x8_cad.c) chops pixel data into
 * runs of at most 24 bytes and prefixes each with one 0x40 control byte — 25 — while a
 * command transfer is one 0x00 control byte, the command, and this controller's longest
 * argument list, which is two (u8x8_d_ssd1306_128x64_noname.c's draw-tile path). The guard
 * below turns an overflow into a dropped frame and one log line rather than a smashed
 * stack, because that ceiling belongs to a vendored file a future `cp -R` could move. */
#define XFER_MAX 32
static uint8_t s_buf[XFER_MAX];
static uint8_t s_len;
static bool    s_overflow;

/* A 25-byte transfer at 400 kHz is about 0.6 ms on the wire. 50 ms is 80x that, and it is a
 * bound rather than an expectation: an absent panel NACKs immediately, but a bus held low by
 * a miswired or unpowered module would otherwise park this task on it. The display task's
 * whole period is 200 ms and a full redraw is nine transfers, so a bus in that state costs
 * the task its cadence — which is the correct thing to lose, and the only thing. */
#define XFER_TIMEOUT_MS 50

/* One log line per outage, not one per failed transfer: a missing panel fails nine times a
 * frame, five times a second, and a flood on a 115200-baud console is how the useful lines
 * get lost. The count is carried so the recovery line can say how much was missed. */
static uint32_t s_fail_count;
static bool     s_fail_logged;

static void note_failure(esp_err_t err)
{
    s_fail_count++;
    if (!s_fail_logged) {
        ESP_LOGE(TAG, "panel does not answer at 0x%02x on SDA %d / SCL %d (%s) — "
                      "further failures are counted, not logged",
                 (unsigned)BOARD_OLED_ADDR, (int)BOARD_I2C_SDA, (int)BOARD_I2C_SCL,
                 esp_err_to_name(err));
        s_fail_logged = true;
    }
}

static void note_success(void)
{
    if (s_fail_logged) {
        ESP_LOGI(TAG, "panel answering again after %u failed transfers",
                 (unsigned)s_fail_count);
        s_fail_logged = false;
    }
    s_fail_count = 0;
}

static uint8_t byte_cb(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr)
{
    (void)u8x8;

    switch (msg) {
    case U8X8_MSG_BYTE_INIT:
        /* The bus and the device already exist — display_hal_setup made them before u8g2 was
         * ever handed this callback, because their failure has somewhere to be reported and
         * a return value of 0 from here does not. */
        return 1;

    case U8X8_MSG_BYTE_SET_DC:
        /* I2C has no D/C line. The bit travels as the control byte (0x00 command, 0x40 data)
         * that u8x8's cad layer already puts at the head of every transfer, so this message
         * is genuinely nothing to do rather than something unimplemented. */
        return 1;

    case U8X8_MSG_BYTE_START_TRANSFER:
        s_len = 0;
        s_overflow = false;
        return 1;

    case U8X8_MSG_BYTE_SEND:
        if ((size_t)s_len + arg_int > sizeof(s_buf)) {
            /* Recorded, not truncated: half a transfer would put the controller into a state
             * nothing here could reason about. The whole transfer is dropped at END. */
            s_overflow = true;
            return 0;
        }
        memcpy(s_buf + s_len, arg_ptr, arg_int);
        s_len = (uint8_t)(s_len + arg_int);
        return 1;

    case U8X8_MSG_BYTE_END_TRANSFER: {
        uint8_t len = s_len;
        s_len = 0;
        if (s_overflow) {
            s_overflow = false;
            ESP_LOGE(TAG, "a transfer wanted more than %d bytes — dropped; u8g2's own limit "
                          "moved and XFER_MAX has to follow it", XFER_MAX);
            return 0;
        }
        if (len == 0) return 1;
        if (s_dev == NULL) {
            /* No device to talk to: display_hal_setup failed and said so. Drawing still
             * works — it lands in u8g2's RAM buffer — and the task above keeps its other
             * job (relay_stats_sample) rather than stopping because a panel is missing. */
            return 0;
        }
        /* The 7-bit address is NOT in s_buf. i2c_master_transmit prepends it from the device
         * handle, so u8x8's own i2c_address field (which its cad layer defaults to 0x078)
         * is unused on this port and deliberately never set. */
        esp_err_t err = i2c_master_transmit(s_dev, s_buf, len, XFER_TIMEOUT_MS);
        if (err != ESP_OK) {
            note_failure(err);
            return 0;
        }
        note_success();
        return 1;
    }

    default:
        return 0;
    }
}

static uint8_t gpio_and_delay_cb(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr)
{
    (void)u8x8;
    (void)arg_ptr;

    switch (msg) {
    case U8X8_MSG_GPIO_AND_DELAY_INIT:
        /* Nothing to set up: the panel has no reset, chip-select or D/C line on I2C, and the
         * two bus pins belong to the I2C driver, not to u8g2. */
        return 1;

    case U8X8_MSG_DELAY_MILLI:
        /* The only delay this controller's init sequence asks for. Rounded UP to a whole tick
         * — u8g2 asks for a minimum, and vTaskDelay(0) would yield rather than wait. */
        vTaskDelay((arg_int + portTICK_PERIOD_MS - 1) / portTICK_PERIOD_MS);
        return 1;

    default:
        /* The sub-millisecond delays (10MICRO, 100NANO, NANO, I2C) exist for u8g2's bit-banged
         * transports. This one is hardware I2C, where the peripheral times the bus itself, so
         * they are correctly nothing — as are the GPIO messages for lines this wiring has not
         * got. */
        return 1;
    }
}

esp_err_t display_hal_setup(u8g2_t *u8g2)
{
    /* Set u8g2 up FIRST, unconditionally, so the caller has a usable u8g2_t on every path out
     * of here — including the failures below, where the byte callback becomes a sink. */
    u8g2_Setup_ssd1306_i2c_128x64_noname_f(u8g2, U8G2_R0, byte_cb, gpio_and_delay_cb);

    i2c_master_bus_config_t bus = {
        .i2c_port = -1,                       /* the driver picks a free port; nothing else on
                                                 this firmware uses I2C at all */
        .sda_io_num = BOARD_I2C_SDA,
        .scl_io_num = BOARD_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,               /* the driver's own documented typical value */
        .flags.enable_internal_pullup = true, /* every SSD1306 module carries its own pull-ups;
                                                 these are the weak fallback for a bare panel,
                                                 and the header says as much: not strong enough
                                                 to be relied on at 400 kHz */
    };
    esp_err_t err = i2c_new_master_bus(&bus, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus on SDA %d / SCL %d refused (%s) — the panel stays dark and "
                      "board.h is the file to correct",
                 (int)BOARD_I2C_SDA, (int)BOARD_I2C_SCL, esp_err_to_name(err));
        return err;
    }

    i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BOARD_OLED_ADDR,
        .scl_speed_hz = BOARD_I2C_HZ,
    };
    err = i2c_master_bus_add_device(s_bus, &dev, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cannot address the panel at 0x%02x (%s)",
                 (unsigned)BOARD_OLED_ADDR, esp_err_to_name(err));
        s_dev = NULL;
        return err;
    }

    return ESP_OK;
}
