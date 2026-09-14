#include "i2c_bus.h"
#include "esp_check.h"

static const char *TAG = "i2c_bus";
static i2c_master_bus_handle_t s_bus;

esp_err_t i2c_bus_init(int sda_pin, int scl_pin) {
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = sda_pin,
        .scl_io_num = scl_pin,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_bus), TAG, "I2C bus init failed");
    return ESP_OK;
}

i2c_master_bus_handle_t i2c_bus_handle(void) { return s_bus; }

esp_err_t i2c_bus_recover(void) {
    if (s_bus == NULL) return ESP_ERR_INVALID_STATE;
    return i2c_master_bus_reset(s_bus);
}
