#include "pca9685.h"
#include "board.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"

static const char *TAG = "pca9685";

#define PCA9685_MODE1       0x00
#define PCA9685_PRESCALE    0xFE
#define PCA9685_LED0_ON_L   0x06

#define PCA9685_MODE1_RESTART  0x80
#define PCA9685_MODE1_AI       0x20
#define PCA9685_MODE1_SLEEP    0x10

// Two boards, one per axle. Index 0 is the front axle, 1 the rear; a logical channel picks
// between them by division. Their names in the log are worth the two strings: at bring-up
// "rear board did not answer" is a wiring fault you can act on, "PCA9685 init failed" is not.
#define PCA_COUNT 2
static const uint8_t s_addr[PCA_COUNT] = { BOARD_PCA_ADDR_FRONT, BOARD_PCA_ADDR_REAR };
static const char   *s_name[PCA_COUNT] = { "front", "rear" };

static i2c_master_bus_handle_t bus_handle;
static i2c_master_dev_handle_t s_dev[PCA_COUNT];

static esp_err_t pca9685_write_reg(int idx, uint8_t reg, uint8_t value) {
    uint8_t buf[2] = {reg, value};
    return i2c_master_transmit(s_dev[idx], buf, sizeof(buf), -1);
}

static esp_err_t pca9685_read_reg(int idx, uint8_t reg, uint8_t *value) {
    return i2c_master_transmit_receive(s_dev[idx], &reg, 1, value, 1, -1);
}

esp_err_t pca9685_bus_init(int sda_pin, int scl_pin, uint32_t i2c_speed_hz) {
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = sda_pin,
        .scl_io_num = scl_pin,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &bus_handle), TAG, "I2C bus init failed");

    for (int i = 0; i < PCA_COUNT; i++) {
        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = s_addr[i],
            .scl_speed_hz = i2c_speed_hz,
        };
        ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus_handle, &dev_cfg, &s_dev[i]),
                            TAG, "%s PCA9685 (0x%02x) add failed", s_name[i], s_addr[i]);
    }
    return ESP_OK;
}

esp_err_t pca9685_init(uint16_t pwm_freq_hz) {
    uint8_t prescale = (uint8_t)((25000000.0 / (4096.0 * pwm_freq_hz)) - 0.5);
    ESP_LOGI(TAG, "PCA9685 prescale = %d for %d Hz", prescale, pwm_freq_hz);

    for (int i = 0; i < PCA_COUNT; i++) {
        ESP_RETURN_ON_ERROR(pca9685_write_reg(i, PCA9685_MODE1, PCA9685_MODE1_SLEEP),
                            TAG, "%s (0x%02x) sleep failed", s_name[i], s_addr[i]);
        vTaskDelay(pdMS_TO_TICKS(5));
        ESP_RETURN_ON_ERROR(pca9685_write_reg(i, PCA9685_PRESCALE, prescale),
                            TAG, "%s (0x%02x) prescale failed", s_name[i], s_addr[i]);
        ESP_RETURN_ON_ERROR(pca9685_write_reg(i, PCA9685_MODE1, PCA9685_MODE1_AI),
                            TAG, "%s (0x%02x) wake failed", s_name[i], s_addr[i]);
        vTaskDelay(pdMS_TO_TICKS(5));

        uint8_t mode1;
        ESP_RETURN_ON_ERROR(pca9685_read_reg(i, PCA9685_MODE1, &mode1),
                            TAG, "%s (0x%02x) mode1 read failed", s_name[i], s_addr[i]);
        ESP_RETURN_ON_ERROR(pca9685_write_reg(i, PCA9685_MODE1, mode1 | PCA9685_MODE1_RESTART),
                            TAG, "%s (0x%02x) restart failed", s_name[i], s_addr[i]);

        ESP_LOGI(TAG, "%s PCA9685 (0x%02x) initialized", s_name[i], s_addr[i]);
    }
    return ESP_OK;
}

esp_err_t pca9685_set_pwm(uint8_t channel, uint16_t duty) {
    // Logical 0..7 across two boards. Anything higher would silently land on another board's
    // channel or on the ALL_LED registers, so it is rejected rather than wrapped.
    if (channel >= PCA_COUNT * BOARD_PCA_CH_PER_CHIP) return ESP_ERR_INVALID_ARG;
    int idx = channel / BOARD_PCA_CH_PER_CHIP;
    uint8_t phys = channel % BOARD_PCA_CH_PER_CHIP;

    uint8_t base_reg = PCA9685_LED0_ON_L + 4 * phys;
    uint16_t on = 0;
    uint16_t off = duty;

    if (duty == 0) {
        on = 0; off = 0x1000;
    } else if (duty >= 4095) {
        on = 0x1000; off = 0;
    }

    uint8_t buf[5] = {
        base_reg,
        on & 0xFF, (on >> 8) & 0x1F,
        off & 0xFF, (off >> 8) & 0x1F,
    };
    return i2c_master_transmit(s_dev[idx], buf, sizeof(buf), -1);
}
