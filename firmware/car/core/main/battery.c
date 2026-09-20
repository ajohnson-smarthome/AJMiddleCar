#include "battery.h"
#include "battery_soc.h"
#include "power_monitor.h"
#include "board.h"
#include "i2c_bus.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "battery";

/* The one snapshot, written by the battery task and read by whoever gathers telemetry —
   the rt_link task for the 5 Hz push (priority 6) and the httpd task for /status. A
   critical section rather than aligned loads: five fields must be read as one reading, or
   a frame could carry `absent` with the numbers of the read before, or one read's voltage
   beside the next one's current. */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static battery_snapshot_t s_snap = { .state = BATTERY_STATE_ABSENT, .present = false, .soc = -1 };

static void publish(const battery_snapshot_t *s) {
    taskENTER_CRITICAL(&s_mux);
    s_snap = *s;
    taskEXIT_CRITICAL(&s_mux);
}

void battery_snapshot(battery_snapshot_t *out) {
    taskENTER_CRITICAL(&s_mux);
    *out = s_snap;
    taskEXIT_CRITICAL(&s_mux);
}

static void battery_task(void *arg) {
    (void)arg;
    battery_rule_t rule;
    battery_soc_t  soc;
    battery_rule_init(&rule);
    battery_soc_init(&soc);
    /* When the previous good read was taken, for the coulomb count's dt. 0 is "no previous
       reading" — after boot and after `absent` — and the first step then takes the nominal
       period: the time a module spent not answering is not a stretch the current flowed
       through this count. */
    int64_t last_us = 0;
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        power_sample_t s;
        esp_err_t e = power_monitor_read(&s);
        if (e != ESP_OK) {
            if (battery_rule_failed(&rule)) {
                ESP_LOGW(TAG, "%d reads in a row failed (%s) — battery absent; still trying",
                         BATTERY_ABSENT_AFTER, esp_err_to_name(e));
            }
            /* One or two failures leave the last reading standing — the pult sees the last
               numbers, not a flicker to null. Three publish `absent`, every time, so the
               snapshot cannot lag the word. */
            if (battery_rule_absent(&rule)) {
                battery_snapshot_t gone = { .state = BATTERY_STATE_ABSENT, .present = false, .soc = -1 };
                publish(&gone);
            }
        } else {
            int64_t now = esp_timer_get_time();
            if (battery_rule_absent(&rule)) {
                /* Back — or here for the first time: the remainder starts as after a boot,
                   from the rest voltage, whatever count the module left with. */
                battery_soc_reset(&soc);
                last_us = 0;
                ESP_LOGI(TAG, "monitor answering: %ld mV, %ld mA — measuring",
                         (long)s.mv, (long)s.ma);
            }
            uint32_t dt_ms = last_us ? (uint32_t)((now - last_us) / 1000) : BATTERY_PERIOD_MS;
            last_us = now;
            int pct = battery_soc_step(&soc, s.mv, s.ma, dt_ms);
            battery_rule_read(&rule, pct);
            battery_snapshot_t snap = {
                .state = battery_rule_word(&rule), .present = true,
                .mv = s.mv, .ma = s.ma, .mw = s.mw, .soc = pct,
            };
            publish(&snap);
        }
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(BATTERY_PERIOD_MS));
    }
}

esp_err_t battery_init(void) {
    if (i2c_bus_handle() == NULL) {
        /* Nothing to read from and nothing to retry: the bus is created once at boot. The
           snapshot's initial value already says `absent`. */
        ESP_LOGW(TAG, "no I2C bus — battery absent");
        return ESP_OK;
    }
    esp_err_t e = power_monitor_init();
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "no battery monitor at 0x%02x (%s) — absent until one answers",
                 BOARD_INA260_ADDR, esp_err_to_name(e));
    }
    /* Priority 3, with the video tasks: below the actuator (5) and rt_link (6), so a read
       that waits on the bus waits behind the actuator's write, never the other way round. */
    if (xTaskCreate(battery_task, "battery", 3072, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "no battery task — battery absent");
        return ESP_FAIL;
    }
    return ESP_OK;
}
