#include <stdio.h>
#include <string.h>
#include <math.h>
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
#include "nvs.h"
#include "radio_ota.h"
#include "radio_flash.h"
#include "radio_expected.h"

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
    // NaN fails every comparison below, so without isfinite `mix nan nan` was accepted
    // and rode a formally-undefined float->uint16 cast to a lucky stop.
    if (!isfinite(*t) || !isfinite(*y)) return -1;
    if (*t < -1.0f || *t > 1.0f || *y < -1.0f || *y > 1.0f) return -1;
    return 0;
}

/* The attempt counter. Its own namespace and a single byte: this is not configuration, nothing
   reads it over the API, and the JSON-per-domain shape the config domains use would be
   ceremony around one number. */
#define RADIO_NVS_NS   "radio"
#define RADIO_NVS_KEY  "ota_tries"

static uint8_t radio_attempts_load(void) {
    nvs_handle_t h;
    if (nvs_open(RADIO_NVS_NS, NVS_READONLY, &h) != ESP_OK) return 0;
    uint8_t n = 0;
    if (nvs_get_u8(h, RADIO_NVS_KEY, &n) != ESP_OK) n = 0;
    nvs_close(h);
    return n;
}

static void radio_attempts_store(uint8_t n) {
    nvs_handle_t h;
    if (nvs_open(RADIO_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "cannot open NVS for the radio attempt counter");
        return;
    }
    if (nvs_set_u8(h, RADIO_NVS_KEY, n) == ESP_OK) nvs_commit(h);
    nvs_close(h);
}

static void radio_gate(void) {
    const char *running = radio_flash_version();
    const bool have_image = radio_flash_image_size() > 0;
    const bool match = (strcmp(running, RADIO_EXPECTED_FW) == 0);
    const uint8_t attempts = radio_attempts_load();

    /* Charged BEFORE the flash, deliberately: radio_flash_apply() restarts the car, so a
       counter written after it would never be written at all. */
    const uint8_t next = (uint8_t)radio_ota_next_attempts(match, attempts);
    if (next != attempts) radio_attempts_store(next);

    if (!radio_ota_should_flash(running, RADIO_EXPECTED_FW,
                                attempts, RADIO_OTA_MAX_ATTEMPTS, have_image)) {
        if (!match && have_image) {
            ESP_LOGW(TAG, "radio %s vs expected %s: %d attempts spent, giving up and booting on",
                     running, RADIO_EXPECTED_FW, (int)attempts);
        }
        return;
    }
    ESP_LOGW(TAG, "radio %s, expected %s — flashing (attempt %d of %d)",
             running, RADIO_EXPECTED_FW, (int)next, RADIO_OTA_MAX_ATTEMPTS);
    radio_flash_apply();   /* does not return */
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
        /* The migration erases the bench-earned calibration and every config domain.
           There is no snapshot/restore (deferred — 2026-08-23 audit); the flag is how
           the app tells the user the wizard must be re-run, instead of silence. */
        ESP_LOGW(TAG, "NVS format changed — erasing (calibration and config are lost)");
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs = nvs_flash_init();
        status_api_note_nvs_wiped();
    }
    ESP_ERROR_CHECK(nvs);

    ESP_ERROR_CHECK(ramp_init());   // ramp_ms must be loaded before the actuator task ticks
    ESP_ERROR_CHECK(link_init());   // the sole PCA9685 writer; zeroes the chip at boot
    car_init();
    wheel_init();                          // load wheel/encoder params (NVS or defaults)
    dims_init();                           // load car dimensions (NVS or defaults)
    ESP_ERROR_CHECK(wifi_ap_start(CAR_AP_SSID, CAR_AP_PASS));
    /* OTA rollback: the property rollback protects is "the car is reachable" — the AP is
       up, so mark the image valid NOW, before the API registrations and before
       status_api_start's radio-version RPC (up to 5 s against a mismatched slave). One
       reset inside that window used to silently revert a good update. The tradeoff is
       deliberate: an image that fails beyond this line boot-loops WITHOUT rollback, so
       everything below must tolerate failure — a panic after this point is its own bug. */
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK &&
        ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
    }
    /* The radio's image rides inside this one, so a pin bump no longer means a bench visit
       (docs/superpowers/specs/2026-08-31-radio-in-one-image-design.md).

       The position is not free. AFTER mark-valid, because a successful flash ends in a restart
       we chose and the bootloader cannot tell that from a crash — before it, that restart would
       revert a perfectly good app image. BEFORE rt_link_start/http_server_start, because from
       there on the car is serving and the SDIO link is not ours to drop. */
    radio_gate();
    telemetry_start();                     // 1 Hz RSSI sampler, off the control task
    recovery_init();                       // breadcrumb buffer; the watchdog trips into it
    /* Driving comes up before the API: rt_link carries control, the watchdog and
       telemetry, and none of it depends on the HTTP server being there.
       Post-mark-valid: a failure here must NOT panic — rollback is already waived, so a
       panic is a permanent boot-loop on a car with no cable. Log loudly and keep what runs. */
    esp_err_t err;
    if ((err = rt_link_start()) != ESP_OK)
        ESP_LOGE(TAG, "rt_link_start failed: %s — control channel is down", esp_err_to_name(err));
    if ((err = http_server_start()) != ESP_OK)
        ESP_LOGE(TAG, "http_server_start failed: %s — HTTP API is down", esp_err_to_name(err));
    if ((err = calib_api_start()) != ESP_OK)
        ESP_LOGE(TAG, "calib_api_start failed: %s — calibration endpoint is down", esp_err_to_name(err));
    if ((err = status_api_start()) != ESP_OK)
        ESP_LOGE(TAG, "status_api_start failed: %s — /status endpoint is down", esp_err_to_name(err));
    if ((err = ota_api_start()) != ESP_OK)
        ESP_LOGE(TAG, "ota_api_start failed: %s — OTA endpoint is down, this car cannot be updated over the air", esp_err_to_name(err));
    if ((err = cfg_api_start()) != ESP_OK)
        ESP_LOGE(TAG, "cfg_api_start failed: %s — config endpoints are down, all five domains", esp_err_to_name(err));

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
