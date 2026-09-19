#include "link.h"
#include <stdatomic.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "board.h"
#include "pca9685.h"
#include "ramp.h"

static const char *TAG = "link";

/* Releases owed to sources that could not take s_lock twice, keyed by the SERIAL of the grant
   they were aimed at (0 = nothing owed). Drained by link_task under the lock it takes every
   tick, so no grant can outlive its owner's attempt to give it up.

   Keyed on the serial and not just the source, because the first version was, and that let a
   queued release land on the WRONG grant: source S loses both lock races, its bit is queued,
   S obtains a fresh grant before the next tick, the drain sees owner == S and releases the new
   one. For OTA that reopened the actuator to the RT stream for the length of a flash. A grant
   is now released only if it is still the grant that was there when the release was asked
   for; a newer one from the same source is that source's own to give up. */
#define LINK_SRC_COUNT ((int)LINK_SRC_SAFE + 1)
static _Atomic uint32_t s_release_pending[LINK_SRC_COUNT];

static SemaphoreHandle_t s_lock;   /* guards s_arb and s_target */
static link_arb_t        s_arb = { .owner = LINK_SRC_NONE, .until_ms = 0, .sticky = false };
static uint16_t          s_target[8];
static link_tick_t       s_tick;   /* the shadows and the retry pacing; link_task's alone */
static volatile bool     s_bus_ok = true;
/* Published under the lock, read without it. Telemetry is gathered on the rt_link task,
   which holds the car's safety and must not block on a mutex for a value it only
   prints; blocking there also meant a timeout reported "none" while someone was
   actively holding. */
static volatile link_src_t s_owner_pub = LINK_SRC_NONE;
/* The serial of the grant s_owner_pub describes, published beside it for link_release_must,
   which has just failed to take the lock and needs to say WHICH grant it meant. */
static _Atomic uint32_t    s_serial_pub;

static uint32_t now_ms(void) {
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

/* The tick's effects table (link.h): the real boards behind the pure tick. The last
   driver error is kept for the log line the bus reset earns. */
static esp_err_t s_last_err = ESP_OK;

static bool fx_ready(void *ctx)    { (void)ctx; return pca9685_ready(); }
static void fx_zero_all(void *ctx) { (void)ctx; pca9685_zero_all(); }
static bool fx_init(void *ctx) {
    (void)ctx;
    esp_err_t e = pca9685_init(BOARD_PWM_HZ);
    if (e != ESP_OK) s_last_err = e;
    return e == ESP_OK;
}
static bool fx_set_pwm(void *ctx, uint8_t ch, uint16_t duty) {
    (void)ctx;
    esp_err_t e = pca9685_set_pwm(ch, duty);
    if (e != ESP_OK) s_last_err = e;
    return e == ESP_OK;
}
static void fx_bus_recover(void *ctx) { (void)ctx; pca9685_bus_recover(); }
static uint32_t fx_now_ms(void *ctx)  { (void)ctx; return now_ms(); }

static const link_tick_fx_t s_fx = {
    .ctx = NULL,
    .ready = fx_ready, .zero_all = fx_zero_all, .init = fx_init,
    .set_pwm = fx_set_pwm, .bus_recover = fx_bus_recover, .now_ms = fx_now_ms,
};

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
    atomic_store(&s_serial_pub, s_arb.serial);
    xSemaphoreGive(s_lock);

    if (!granted) {
        /* Rate-limited: a refused source is usually refused at its own frame rate. */
        /* Seeded past the window, not at 0. The dongle's api_guard.c makes the same point for
           the same idiom: a guard's very first rejections — in the first second after boot —
           are exactly the interesting ones, and last_log starting at 0 silently drops whichever
           of them land before now_ms() first exceeds 1000. */
        static uint32_t last_log = (uint32_t)-1001;
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

bool link_release_must(link_src_t src) {
    if (src < 0 || src >= LINK_SRC_COUNT) {
        /* LINK_SRC_NONE, or garbage. No caller passes it today, but link_owner() returns it and
           link_release_must(link_owner()) is a natural thing to write; indexing the queue with
           -1 would be undefined behaviour with no diagnostic. Nothing owns "nothing", so there
           is nothing to release, and true is the honest answer. */
        return true;
    }
    if (link_release(src)) return true;
    vTaskDelay(1);   /* the lock is held across a memcpy, never across a wait */
    if (link_release(src)) return true;

    /* Hand it to the 50 Hz task instead of giving up. Losing both races means colliding with
       whoever holds s_lock — and for most of a tick that is the actuator task itself, which
       takes the lock every pass and can finish this under a lock it already owns. Before this,
       a caller that lost both races left a sticky top-rank grant (SAFE after a goodbye, OTA on
       a failure path) standing with nothing in the system able to take it back: every later
       car_drive refused until a power cycle. Thirteen of the fifteen call sites dropped this
       return on the floor, which link.h warned against — the warning was right, and needing it
       was the real defect. A queued release completes within one tick. */
    /* The serial as last published. If a link_set is granting a NEWER one right now, this
       release is aimed at the old grant — which is gone — and the drain will correctly do
       nothing; whoever took the new grant releases it. */
    uint32_t serial = atomic_load(&s_serial_pub);
    atomic_store(&s_release_pending[src], serial != 0u ? serial : 1u);
    ESP_LOGW(TAG, "%s could not take the lock to release the actuator — queued for the "
                  "actuator task", link_src_name(src));
    return false;
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
        vTaskDelayUntil(&last, pdMS_TO_TICKS(LINK_TICK_MS));
        if (twdt) esp_task_wdt_reset();

        uint16_t   tgt[8];
        link_src_t owner;
        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(LINK_TICK_MS)) != pdTRUE) continue;

        /* Finish the releases link_release_must could not. Drained before the lapse check
           below, so a grant given up this tick falls to the safe target on this tick rather
           than on the next one. */
        for (int src = 0; src < LINK_SRC_COUNT; src++) {
            uint32_t want = atomic_exchange(&s_release_pending[src], 0u);
            if (want == 0u) continue;
            if (s_arb.owner == (link_src_t)src && s_arb.serial == want) {
                link_arb_release(&s_arb, (link_src_t)src);
                memset(s_target, 0, sizeof(s_target));
                s_owner_pub = LINK_SRC_NONE;
            }
            /* Otherwise the grant this was aimed at is already gone — released by its owner
               on the retry, lapsed, or replaced by a newer one. Nothing to do, and nothing
               to keep. */
        }
        /* An expired grant means nobody is driving: fall to zero rather than holding
           the last command, which is what "ownership lapses" has to mean physically. */
        if (link_arb_lapsed(&s_arb, now_ms())) {
            s_arb.owner = LINK_SRC_NONE;
            memset(s_target, 0, sizeof(s_target));
            s_owner_pub = LINK_SRC_NONE;
        }
        memcpy(tgt, s_target, sizeof(tgt));
        owner = s_arb.owner;   /* read with the target it belongs to: the ramp depends on it */
        xSemaphoreGive(s_lock);

        /* The whole I2C side of the tick is link_tick_io (link.h), pure over s_fx, so that
           the bring-up retry and the write pass are host-tested; what is left here is the
           bus word the rest of the car reads and the log lines. */
        uint16_t up = link_max_up(owner, ramp_get_ms(), LINK_TICK_MS);
        switch (link_tick_io(&s_tick, tgt, up, &s_fx)) {
            case LINK_TICK_WAITING:
                s_bus_ok = false;
                break;
            case LINK_TICK_STILL_DOWN: {
                s_bus_ok = false;
                /* Once per ten seconds, not per attempt: boards left unpowered are a normal
                   state of the bench, and pca9685_init already names the failing write. Seeded
                   past the window so the first attempt's line is not the one dropped. */
                static uint32_t last_log = (uint32_t)-10001;
                uint32_t t = now_ms();
                if ((uint32_t)(t - last_log) > 10000) {
                    last_log = t;
                    ESP_LOGW(TAG, "PCA9685 boards are not up (%s) — retrying once a second, "
                                  "resetting the I2C bus before each attempt",
                             esp_err_to_name(s_last_err));
                }
                break;
            }
            case LINK_TICK_CAME_UP:
                s_bus_ok = false;   /* ok only once the next tick's zeros have landed */
                ESP_LOGI(TAG, "PCA9685 boards are up");
                break;
            case LINK_TICK_IDLE:
                break;
            case LINK_TICK_WROTE:
                s_bus_ok = true;
                break;
            case LINK_TICK_FAILED:
                s_bus_ok = false;
                break;
            case LINK_TICK_RESET:
                s_bus_ok = false;
                ESP_LOGE(TAG, "PCA9685 write failing (%s) — reset the I2C bus",
                         esp_err_to_name(s_last_err));
                break;
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
    link_tick_init(&s_tick);   /* every shadow unknown: the first tick writes eight zeros */
    memset(s_target, 0, sizeof(s_target));

    return xTaskCreate(link_task, "link", 3072, NULL, 5, NULL) == pdPASS ? ESP_OK : ESP_FAIL;
}
