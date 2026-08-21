#include "link.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "pca9685.h"
#include "ramp.h"

static const char *TAG = "link";

#define TICK_MS 20                 /* 50 Hz */
#define SHADOW_UNKNOWN 0xFFFFu     /* forces a real write on the first tick */

static SemaphoreHandle_t s_lock;   /* guards s_arb and s_target */
static link_arb_t        s_arb = { .owner = LINK_SRC_NONE, .until_ms = 0, .sticky = false };
static uint16_t          s_target[8];
static uint16_t          s_current[8];
static volatile bool     s_bus_ok = true;
/* Published under the lock, read without it. Telemetry runs in an esp_timer callback
   at priority 22 and must not block on a mutex for a value it only prints; blocking
   there also meant a timeout reported "none" while someone was actively holding. */
static volatile link_src_t s_owner_pub = LINK_SRC_NONE;

static uint32_t now_ms(void) {
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

bool link_bus_ok(void) { return s_bus_ok; }

link_src_t link_owner(void) { return s_owner_pub; }

bool link_set(link_src_t src, const uint16_t duty[8], uint32_t hold_ms, bool sticky) {
    if (!s_lock) return false;
    /* A short wait, not the 200 ms the old car_drive used: this lock is held only for
       a memcpy and a struct assignment, so anything longer means something is wrong,
       and a control frame is better dropped than delayed a fifth of a second. */
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) != pdTRUE) {
        ESP_LOGW(TAG, "%s: lock busy, command dropped", link_src_name(src));
        return false;
    }
    bool granted = link_arb_grant(&s_arb, src, now_ms(), hold_ms, sticky);
    if (granted) {
        memcpy(s_target, duty, sizeof(s_target));
    }
    link_src_t owner = s_arb.owner;
    s_owner_pub = owner;
    xSemaphoreGive(s_lock);

    if (!granted) {
        /* Rate-limited: a refused source is usually refused at its own frame rate. */
        static uint32_t last_log;
        uint32_t t = now_ms();
        if ((uint32_t)(t - last_log) > 1000) {
            last_log = t;
            ESP_LOGW(TAG, "%s refused: %s holds the actuator",
                     link_src_name(src), link_src_name(owner));
        }
    }
    return granted;
}

bool link_release(link_src_t src) {
    if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) != pdTRUE) return false;
    if (s_arb.owner == src) {
        link_arb_release(&s_arb, src);
        memset(s_target, 0, sizeof(s_target));   /* nobody owns it -> the safe target */
        s_owner_pub = LINK_SRC_NONE;
    }
    xSemaphoreGive(s_lock);
    return true;
}

static void link_task(void *arg) {
    (void)arg;
    /* The only writer to the PCA9685: if this task stops running, the motors keep
       whatever duty they were last given, forever. Let the task watchdog reboot the
       board instead — boot zeroes the chip before anything else can command it. */
    bool twdt = esp_task_wdt_add(NULL) == ESP_OK;
    if (!twdt) ESP_LOGW(TAG, "task watchdog not available");

    TickType_t last = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(TICK_MS));
        if (twdt) esp_task_wdt_reset();

        uint16_t tgt[8];
        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(TICK_MS)) != pdTRUE) continue;
        /* An expired grant means nobody is driving: fall to zero rather than holding
           the last command, which is what "ownership lapses" has to mean physically. */
        if (link_arb_lapsed(&s_arb, now_ms())) {
            s_arb.owner = LINK_SRC_NONE;
            memset(s_target, 0, sizeof(s_target));
            s_owner_pub = LINK_SRC_NONE;
        }
        memcpy(tgt, s_target, sizeof(tgt));
        xSemaphoreGive(s_lock);

        uint16_t up = ramp_max_up_per_tick(ramp_get_ms(), TICK_MS);
        bool wrote = false, failed = false;
        esp_err_t last_err = ESP_OK;
        for (uint8_t ch = 0; ch < 8; ch++) {
            uint16_t next = ramp_step(s_current[ch], tgt[ch], up);
            if (next == s_current[ch]) continue;
            wrote = true;
            esp_err_t e = pca9685_set_pwm(ch, next);
            if (e == ESP_OK) {
                s_current[ch] = next;          /* shadow follows the chip, not our intent */
            } else {
                /* Deliberately leave s_current alone. It still differs from the target,
                   so the next tick retries — where updating it first would have left the
                   firmware believing a spinning motor was stopped, forever. */
                failed = true;
                last_err = e;
            }
        }
        /* Only a tick that actually wrote may call the bus healthy. A tick where every
           channel already sat at its target attempts nothing, and clearing the flag on
           that evidence would report a dead bus as fine the moment the car stood still. */
        if (failed)      s_bus_ok = false;
        else if (wrote)  s_bus_ok = true;

        /* A wedged bus fails eight channels fifty times a second. Logging every one
           saturates a 115200 console so completely that the "boot into the network so
           it is diagnosable" trade buys nothing — so speak once a second, and use that
           same beat to try clocking the bus free. Retrying forever without escalating
           leaves the motors at their last duty until the battery comes off. */
        static uint32_t s_fail_ticks;
        if (failed) {
            s_fail_ticks++;
            if (s_fail_ticks % 50 == 0) {
                ESP_LOGE(TAG, "PCA9685 write failing (%s) — resetting the I2C bus",
                         esp_err_to_name(last_err));
                pca9685_bus_recover();
            }
        } else {
            s_fail_ticks = 0;
        }
    }
}

esp_err_t link_init(void) {
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    /* The chip's registers survive a P4 reset, so "we booted" is not "the motors are
       off". Say so to the hardware before anything else can command it. */
    esp_err_t e = pca9685_zero_all();
    if (e != ESP_OK) {
        s_bus_ok = false;
        ESP_LOGE(TAG, "could not zero the PCA9685 at boot: %s", esp_err_to_name(e));
    }
    for (int ch = 0; ch < 8; ch++) s_current[ch] = SHADOW_UNKNOWN;
    memset(s_target, 0, sizeof(s_target));

    return xTaskCreate(link_task, "link", 3072, NULL, 5, NULL) == pdPASS ? ESP_OK : ESP_FAIL;
}
