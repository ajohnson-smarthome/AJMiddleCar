#include "power_monitor.h"
#include "board.h"
#include "i2c_bus.h"
#include "driver/i2c_master.h"
#include "esp_log.h"

static const char *TAG = "ina260";

/* TI INA260 (SBOS656C): a 2 mΩ shunt with its own ADC, 16-bit words MSB first. The LSBs are
   fixed by the chip — there is no calibration register to write, which is what makes this
   the short driver among the three (research § 3). The bench numbers of 2026-09-20 read
   through these: 0x01 = 56 → 70 mA, 0x02 = 8733 → 10.92 V, 0x03 = 77 → 0.77 W. */
#define INA260_REG_CONFIG    0x00
#define INA260_REG_CURRENT   0x01   /* two's complement, 1.25 mA/LSB */
#define INA260_REG_BUS_V     0x02   /* 1.25 mV/LSB */
#define INA260_REG_POWER     0x03   /* 10 mW/LSB */
#define INA260_REG_MFR_ID    0xFE   /* 0x5449, "TI" */
#define INA260_REG_DIE_ID    0xFF   /* 0x2270 */

#define INA260_MFR_ID  0x5449
#define INA260_DIE_ID  0x2270

/* CONFIG: RST[15] = 0; bits 14:12 written back as they read (110); AVG[11:9] = 010 — 16
   samples; VBUSCT[8:6] = ISHCT[5:3] = 100 — 1.1 ms, the power-on value; MODE[2:0] = 111 —
   shunt current and bus voltage, continuous. 0x6527 against the 0x6127 the chip boots with
   (bringup.md): the one change is the averaging. The motors' 1 kHz PWM puts a sawtooth on
   the current, and 16 conversions of 1.1 ms span ~18 periods of it; if the numbers still
   jump on the bench, 64 (AVG = 011) is the next step (design § Risks). A full cycle — both
   quantities, averaged — is ~35 ms, well inside BATTERY_PERIOD_MS, so a read never waits. */
#define INA260_CONFIG  0x6527

/* Never wait forever on the bus, for the reason pca9685.c gives: this wire is shared with the
   actuator's writes, and a slave holding SDA low must cost this task one timeout, not the
   motors their last duty. */
#define INA260_I2C_TIMEOUT_MS 50

static i2c_master_dev_handle_t s_dev;
/* The chip has answered with its own id and taken CONFIG since the last failure. Cleared by
   any failed transaction, so the next read detects and configures again first. */
static bool s_ready;

static esp_err_t read16(uint8_t reg, uint16_t *out) {
    uint8_t buf[2];
    esp_err_t e = i2c_master_transmit_receive(s_dev, &reg, 1, buf, sizeof(buf), INA260_I2C_TIMEOUT_MS);
    if (e != ESP_OK) return e;
    *out = (uint16_t)((buf[0] << 8) | buf[1]);
    return ESP_OK;
}

static esp_err_t write16(uint8_t reg, uint16_t v) {
    uint8_t buf[3] = { reg, (uint8_t)(v >> 8), (uint8_t)(v & 0xFF) };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), INA260_I2C_TIMEOUT_MS);
}

/* Detect and configure. `loud` only from init: a wrong chip sitting at the address would
   otherwise be reported five times a second for as long as it sits there. */
static esp_err_t bring_up(bool loud) {
    uint16_t mfr = 0, die = 0;
    esp_err_t e = read16(INA260_REG_MFR_ID, &mfr);
    if (e == ESP_OK) e = read16(INA260_REG_DIE_ID, &die);
    if (e != ESP_OK) return e;
    if (mfr != INA260_MFR_ID || die != INA260_DIE_ID) {
        if (loud) {
            ESP_LOGW(TAG, "0x%02x answers with id 0x%04x/0x%04x, not an INA260 (0x%04x/0x%04x)",
                     BOARD_INA260_ADDR, mfr, die, INA260_MFR_ID, INA260_DIE_ID);
        }
        return ESP_ERR_NOT_FOUND;
    }
    e = write16(INA260_REG_CONFIG, INA260_CONFIG);
    if (e != ESP_OK) return e;
    s_ready = true;
    if (loud) {
        ESP_LOGI(TAG, "INA260 at 0x%02x (id 0x%04x/0x%04x): averaging 16 x 1.1 ms, continuous",
                 BOARD_INA260_ADDR, mfr, die);
    }
    return ESP_OK;
}

esp_err_t power_monitor_init(void) {
    i2c_master_bus_handle_t bus = i2c_bus_handle();
    if (bus == NULL) return ESP_ERR_INVALID_STATE;
    if (s_dev == NULL) {
        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = BOARD_INA260_ADDR,
            .scl_speed_hz = BOARD_INA260_HZ,
        };
        esp_err_t e = i2c_master_bus_add_device(bus, &dev_cfg, &s_dev);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "add device 0x%02x: %s", BOARD_INA260_ADDR, esp_err_to_name(e));
            return e;
        }
    }
    s_ready = false;
    return bring_up(true);
}

esp_err_t power_monitor_read(power_sample_t *out) {
    if (s_dev == NULL) return ESP_ERR_INVALID_STATE;
    if (!s_ready) {
        esp_err_t e = bring_up(false);
        if (e != ESP_OK) return e;
    }
    uint16_t cur = 0, bus = 0, pwr = 0;
    esp_err_t e = read16(INA260_REG_CURRENT, &cur);
    if (e == ESP_OK) e = read16(INA260_REG_BUS_V, &bus);
    if (e == ESP_OK) e = read16(INA260_REG_POWER, &pwr);
    if (e != ESP_OK) {
        s_ready = false;
        return e;
    }
    /* 1.25 per LSB is 5/4 — integer, exact for every fourth count and truncated toward zero
       for the rest, on both signs of the current. Power's LSB is a whole 10 mW. */
    out->ma = (int32_t)(int16_t)cur * 5 / 4;
    out->mv = (int32_t)bus * 5 / 4;
    out->mw = (int32_t)pwr * 10;
    return ESP_OK;
}
