#include "recovery.h"
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "cJSON.h"
#include "cfg_json.h"
#include "car.h"

static const char *TAG = "recovery";

#define FRAME_HZ      10                              // WS stream rate (phone streams held cmd at 10 Hz)
#define WINDOW_MAX_S  (RECOVER_WIN_MAX_MS / 1000)     // 10
#define MAX_SAMPLES   (WINDOW_MAX_S * FRAME_HZ * 3 / 2) // 150: 10 s @10 Hz + 50% jitter headroom
#define TICK_MS       30                              // replay granularity / reconnect-abort latency
#define TAIL_MS       400                             // cap for the newest segment's reverse duration
#define MOVE_EPS      0.02f                           // below this a sample counts as "stationary"

typedef struct { float t, y; uint32_t ts; } sample_t;

static sample_t        s_buf[MAX_SAMPLES];
static int             s_head = 0;     // next write index
static int             s_count = 0;    // valid samples
static volatile uint32_t s_seq = 0;    // bumped per frame; liveness signal
static bool            s_enabled = true;
static uint16_t        s_window_ms = 5000;
static TaskHandle_t    s_task = NULL;
static portMUX_TYPE    s_mux = portMUX_INITIALIZER_UNLOCKED;

static uint32_t now_ms(void) {
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

void recovery_set_config(bool enabled, uint16_t window_ms) {
    if (window_ms < RECOVER_WIN_MIN_MS) window_ms = RECOVER_WIN_MIN_MS;
    if (window_ms > RECOVER_WIN_MAX_MS) window_ms = RECOVER_WIN_MAX_MS;
    taskENTER_CRITICAL(&s_mux);
    s_enabled = enabled;
    s_window_ms = window_ms;
    taskEXIT_CRITICAL(&s_mux);
}

void recovery_get_config(bool *enabled, uint16_t *window_ms) {
    taskENTER_CRITICAL(&s_mux);
    if (enabled) *enabled = s_enabled;
    if (window_ms) *window_ms = s_window_ms;
    taskEXIT_CRITICAL(&s_mux);
}

void recovery_note_command(float t, float y) {
    uint32_t now = now_ms();
    taskENTER_CRITICAL(&s_mux);
    s_buf[s_head] = (sample_t){ .t = t, .y = y, .ts = now };
    s_head = (s_head + 1) % MAX_SAMPLES;
    if (s_count < MAX_SAMPLES) s_count++;
    s_seq++;
    taskEXIT_CRITICAL(&s_mux);
}

// Snapshot in-window samples newest→oldest into out[] (cap MAX_SAMPLES). Returns count.
// *seq receives the liveness sequence at snapshot time.
static int snapshot(sample_t *out, uint32_t now, uint32_t *seq) {
    int n = 0;
    taskENTER_CRITICAL(&s_mux);
    *seq = s_seq;
    uint16_t win = s_window_ms;
    for (int k = 0; k < s_count; k++) {
        int idx = (s_head - 1 - k + MAX_SAMPLES) % MAX_SAMPLES;  // newest → oldest
        if (recovery_evict(s_buf[idx].ts, now, win)) break;       // older than window → stop
        out[n++] = s_buf[idx];
    }
    taskEXIT_CRITICAL(&s_mux);
    return n;
}

static bool any_motion(const sample_t *s, int n) {
    for (int i = 0; i < n; i++) {
        if (s[i].t > MOVE_EPS || s[i].t < -MOVE_EPS ||
            s[i].y > MOVE_EPS || s[i].y < -MOVE_EPS) return true;
    }
    return false;
}

static void retreat_task(void *arg) {
    (void)arg;
    static sample_t snap[MAX_SAMPLES];   // task-owned; not on the small task stack
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);   // wait for a link-loss trigger

        uint32_t t_loss = now_ms();
        uint32_t snap_seq;
        int n = snapshot(snap, t_loss, &snap_seq);
        if (n == 0 || !any_motion(snap, n)) { car_stop(); continue; }

        ESP_LOGW(TAG, "link lost — retracing %d samples in reverse", n);
        bool aborted = false;
        for (int i = 0; i < n && !aborted; i++) {
            float rt, ry;
            recovery_reverse(snap[i].t, snap[i].y, &rt, &ry);
            uint32_t dur = (i == 0)
                ? (uint32_t)(t_loss - snap[0].ts)            // newest held until link loss
                : (uint32_t)(snap[i - 1].ts - snap[i].ts);   // until the next-newer frame
            if (i == 0 && dur > TAIL_MS) dur = TAIL_MS;       // cap the open segment
            car_drive(rt, ry);
            for (uint32_t waited = 0; waited < dur; ) {
                if (s_seq != snap_seq) { aborted = true; break; }  // a frame arrived → link back
                uint32_t step = (dur - waited < TICK_MS) ? (dur - waited) : TICK_MS;
                vTaskDelay(pdMS_TO_TICKS(step));
                waited += step;
            }
        }
        if (aborted) ESP_LOGI(TAG, "link returned — handing control back");
        else { car_stop(); ESP_LOGI(TAG, "retrace exhausted — stopped"); }
    }
}

void recovery_on_link_lost(void) {
    if (!s_enabled) { car_stop(); return; }   // feature off → plain stop (old watchdog behavior)
    if (s_task) xTaskNotifyGive(s_task);       // hand off to the retreat task
    else car_stop();
}

void recovery_save(void) {
    bool en; uint16_t win;
    recovery_get_config(&en, &win);
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"enabled\":%s,\"window_ms\":%u}", en ? "true" : "false", win);
    cfg_json_save("recover", buf);
}

void recovery_init(void) {
    char buf[64];
    if (cfg_json_load("recover", buf, sizeof(buf))) {
        cJSON *j = cJSON_Parse(buf);
        cJSON *je = cJSON_GetObjectItemCaseSensitive(j, "enabled");
        int win;
        if (cJSON_IsBool(je)) s_enabled = cJSON_IsTrue(je);
        if (cfg_json_int(j, "window_ms", &win) && win >= RECOVER_WIN_MIN_MS && win <= RECOVER_WIN_MAX_MS)
            s_window_ms = (uint16_t)win;
        cJSON_Delete(j);
    }
    BaseType_t ok = xTaskCreate(retreat_task, "recovery", 3072, NULL, 5, &s_task);
    if (ok != pdPASS) ESP_LOGE(TAG, "retreat task create failed");
    ESP_LOGI(TAG, "recovery %s, window %u ms", s_enabled ? "on" : "off", s_window_ms);
}
