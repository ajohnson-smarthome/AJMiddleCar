#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/usb_serial_jtag.h"
#include "esp_log.h"

#include "pca9685.h"
#include "car.h"
#include "nvs_flash.h"
#include "wifi_ap.h"
#include "http_server.h"
#include "ws_control.h"
#include "watchdog.h"
#include "recovery.h"
#include "calib_api.h"
#include "status_api.h"
#include "ota_api.h"
#include "ramp.h"
#include "ramp_api.h"
#include "trim_api.h"
#include "recovery_api.h"
#include "wheel.h"
#include "dims.h"
#include "wheel_api.h"
#include "dims_api.h"
#include "telemetry.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"

static const char *TAG = "main";

#define I2C_SDA_PIN  22
#define I2C_SCL_PIN  23
#define I2C_FREQ_HZ  400000
#define PWM_FREQ_HZ  1000
#define AP_SSID      "ESP32-Car"
#define AP_PASSWORD  "drive1234"   // >= 8 chars for WPA2; "" for open
#define WDT_TIMEOUT_MS 300

static void console_init(void) {
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&cfg));
}

static int read_line(char *buf, size_t maxlen) {
    size_t pos = 0;
    while (pos < maxlen - 1) {
        uint8_t c;
        int n = usb_serial_jtag_read_bytes(&c, 1, portMAX_DELAY);
        if (n <= 0) continue;
        if (c == '\r' || c == '\n') {
            buf[pos] = '\0';
            return (int)pos;
        }
        buf[pos++] = (char)c;
    }
    buf[pos] = '\0';
    return -1;
}

// Parse "mix <t> <y>", t,y in [-1,1]. Returns 0 on success.
static int parse_mix(const char *line, float *t, float *y) {
    char cmd[8];
    if (sscanf(line, "%7s %f %f", cmd, t, y) != 3) return -1;
    if (strcmp(cmd, "mix") != 0) return -1;
    // Reject out-of-range console input early with an error (car_drive also clamps).
    if (*t < -1.0f || *t > 1.0f || *y < -1.0f || *y > 1.0f) return -1;
    return 0;
}

void app_main(void) {
    ESP_ERROR_CHECK(pca9685_bus_init(I2C_SDA_PIN, I2C_SCL_PIN, I2C_FREQ_HZ));
    ESP_ERROR_CHECK(pca9685_init(PWM_FREQ_HZ));

    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs);

    ESP_ERROR_CHECK(ramp_init());   // start the sole PCA9685 writer before any car_* call
    car_init();
    wheel_init();                          // load wheel/encoder params (NVS or defaults)
    dims_init();                           // load car dimensions (NVS or defaults)
    ESP_ERROR_CHECK(wifi_ap_start(AP_SSID, AP_PASSWORD));
    ESP_ERROR_CHECK(http_server_start());
    ESP_ERROR_CHECK(ws_control_start());
    ESP_ERROR_CHECK(calib_api_start());
    ESP_ERROR_CHECK(status_api_start());
    ESP_ERROR_CHECK(ota_api_start());
    ESP_ERROR_CHECK(ramp_api_start());
    ESP_ERROR_CHECK(trim_api_start());
    ESP_ERROR_CHECK(recovery_api_start());
    ESP_ERROR_CHECK(wheel_api_start());
    ESP_ERROR_CHECK(dims_api_start());
    ESP_ERROR_CHECK(telemetry_start());
    recovery_init();                       // breadcrumb buffer; must precede the watchdog
    watchdog_init(WDT_TIMEOUT_MS);

    // OTA rollback: mark this freshly-flashed image valid so the bootloader won't roll back.
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK &&
        ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
    }

    console_init();
    ESP_LOGI(TAG, "Ready. Enter 'mix <throttle> <yaw>' (each -1..1), e.g. 'mix 0.5 0.2':");

    char line[48];
    while (1) {
        printf("> ");
        fflush(stdout);
        int len = read_line(line, sizeof(line));
        if (len <= 0) {
            if (len < 0) ESP_LOGE(TAG, "input overflow");
            continue;
        }
        float t, y;
        if (parse_mix(line, &t, &y) == 0) {
            car_drive(t, y);
        } else {
            ESP_LOGE(TAG, "bad command, expected 'mix <t> <y>' with t,y in [-1,1]");
        }
    }
}
