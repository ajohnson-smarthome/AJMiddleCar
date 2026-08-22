#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/usb_serial_jtag.h"
#include "driver/uart.h"
#include "esp_log.h"

#include "board.h"
#include "identity.h"
#include "pca9685.h"
#include "car.h"
#include "link.h"
#include "nvs_flash.h"
#include "wifi_ap.h"
#include "http_server.h"
#include "rt_link.h"
#include "telemetry.h"
#include "recovery.h"
#include "calib_api.h"
#include "status_api.h"
#include "ota_api.h"
#include "cfg_api.h"
#include "ramp.h"
#include "wheel.h"
#include "dims.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"

static const char *TAG = "main";

// The console follows whichever peripheral ESP-IDF was told to put it on, rather than being
// nailed to USB-Serial-JTAG. That mattered the moment it mattered: on the bench the board's
// native USB stopped enumerating, logs were moved to UART0 through the on-board bridge, and
// input would otherwise have kept waiting on a peripheral nothing was attached to.
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
static void console_init(void) {
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&cfg));
}

static int console_read_byte(uint8_t *c) {
    return usb_serial_jtag_read_bytes(c, 1, portMAX_DELAY);
}
#elif CONFIG_ESP_CONSOLE_UART_DEFAULT
static void console_init(void) {
    // Driver-level reads, for the same reason USB-Serial-JTAG uses them: fgets through the VFS
    // returns immediately and spins the prompt.
    ESP_ERROR_CHECK(uart_driver_install(CONFIG_ESP_CONSOLE_UART_NUM, 256, 0, 0, NULL, 0));
}

static int console_read_byte(uint8_t *c) {
    return uart_read_bytes(CONFIG_ESP_CONSOLE_UART_NUM, c, 1, portMAX_DELAY);
}
#else
#error "No supported console peripheral selected — the mix REPL needs USB-Serial-JTAG or UART"
#endif

static int read_line(char *buf, size_t maxlen) {
    size_t pos = 0;
    while (pos < maxlen - 1) {
        uint8_t c;
        int n = console_read_byte(&c);
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
    /* A dead motor bus must not take the radio with it. Booting into the network
       with bus_ok=false is diagnosable and OTA-recoverable; a boot loop needs a
       USB cable and tells you nothing. */
    bool motors_ok = pca9685_bus_init(BOARD_I2C_SDA, BOARD_I2C_SCL, BOARD_I2C_HZ) == ESP_OK
                  && pca9685_init(BOARD_PWM_HZ) == ESP_OK;
    if (motors_ok) {
        /* Immediately, not later in link_init. pca9685_init ends by writing MODE1 with
           the RESTART bit, which by design resumes every channel at its pre-sleep duty —
           so on a reset taken mid-drive the motors come back at full throttle. Everything
           between here and link_init (an NVS init that may erase flash) would run with
           them spinning. */
        pca9685_zero_all();
    } else {
        ESP_LOGE(TAG, "motor bus did not come up — the car will not drive, "
                      "but the network and OTA will. Check I2C wiring and power.");
    }

    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs);

    ESP_ERROR_CHECK(ramp_init());   // ramp_ms must be loaded before the actuator task ticks
    ESP_ERROR_CHECK(link_init());   // the sole PCA9685 writer; zeroes the chip at boot
    car_init();
    wheel_init();                          // load wheel/encoder params (NVS or defaults)
    dims_init();                           // load car dimensions (NVS or defaults)
    ESP_ERROR_CHECK(wifi_ap_start(CAR_AP_SSID, CAR_AP_PASS));
    telemetry_start();                     // 1 Hz RSSI sampler, off the control task
    recovery_init();                       // breadcrumb buffer; the watchdog trips into it
    /* Driving comes up before the API: rt_link carries control, the watchdog and
       telemetry, and none of it depends on the HTTP server being there. */
    ESP_ERROR_CHECK(rt_link_start());
    ESP_ERROR_CHECK(http_server_start());
    ESP_ERROR_CHECK(calib_api_start());
    ESP_ERROR_CHECK(status_api_start());
    ESP_ERROR_CHECK(ota_api_start());
    ESP_ERROR_CHECK(cfg_api_start());   // all five config domains, from the generated table

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
            if (!car_drive(LINK_SRC_CONSOLE, t, y)) {
                ESP_LOGW(TAG, "refused: %s holds the actuator", link_src_name(link_owner()));
            } else if (t == 0.0f && y == 0.0f) {
                /* A console grant is sticky — that is the documented bench behaviour,
                   so `mix 1 0` runs until told otherwise. But `mix 0 0` means "done",
                   and holding on after that would refuse the auto-return for the rest
                   of the boot. It is the only sticky source a human enters by hand. */
                link_release_must(LINK_SRC_CONSOLE);
            }
        } else {
            ESP_LOGE(TAG, "bad command, expected 'mix <t> <y>' with t,y in [-1,1]");
        }
    }
}
