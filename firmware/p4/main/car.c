#include "car.h"
#include <stdio.h>
#include <stdatomic.h>
#include "esp_log.h"
#include "esp_err.h"
#include "cJSON.h"
#include "link.h"
#include "mixer.h"
#include "motors.h"
#include "calibration.h"
#include "trim.h"
#include "cfg_json.h"

static const char *TAG = "car";

/* The calibration is immutable once published. Writers fill the spare buffer and swap
   the pointer; readers load it and never block. That matters more than it sounds: the
   old code took a mutex with a 200 ms timeout on the control path and, on timeout,
   returned having commanded nothing — so an emergency stop could silently vanish. */
static motors_config_t s_cfg_buf[2] = {
    [0] = {
        .wheels = {
            [POS_FL] = { .channel_pair = 0, .sign = 1 },
            [POS_FR] = { .channel_pair = 1, .sign = 1 },
            [POS_RL] = { .channel_pair = 2, .sign = 1 },
            [POS_RR] = { .channel_pair = 3, .sign = 1 },
        },
        .deadzone = 0.05f,
    },
};
static _Atomic(const motors_config_t *) s_cfg = &s_cfg_buf[0];
static int s_cfg_next = 1;          /* only touched by config writers, which are serialised
                                       by the single httpd task */
static _Atomic int s_trim_pct = 0;  /* [-30..30] */

static float clamp_unit(float v) {
    if (v > 1.0f) return 1.0f;
    if (v < -1.0f) return -1.0f;
    return v;
}

static uint32_t hold_for(link_src_t src) {
    switch (src) {
        case LINK_SRC_RT:    return LINK_HOLD_RT_MS;
        case LINK_SRC_CALIB: return LINK_HOLD_CALIB_MS;
        default:             return 0;   /* sticky sources ignore this */
    }
}

static bool sticky_for(link_src_t src) {
    /* Anything that is not a stream holds until it says otherwise. A console command
       runs until the next one, which is the documented bench behaviour. */
    return src != LINK_SRC_RT && src != LINK_SRC_CALIB;
}

bool car_drive(link_src_t src, float throttle, float yaw) {
    throttle = clamp_unit(throttle);
    yaw = clamp_unit(yaw);
    side_speeds_t s = mixer_mix(throttle, yaw);

    trim_apply(&s.left, &s.right, (float)atomic_load(&s_trim_pct) / 100.0f);
    const motors_config_t *cfg = atomic_load(&s_cfg);
    motor_outputs_t out = motors_plan(s.left, s.right, cfg);

    bool applied = link_set(src, out.duty, hold_for(src), sticky_for(src));
    ESP_LOGD(TAG, "drive[%s] t=%.2f y=%.2f -> L=%.2f R=%.2f %s",
             link_src_name(src), throttle, yaw, s.left, s.right,
             applied ? "applied" : "REFUSED");
    return applied;
}

void car_stop(link_src_t src) {
    car_drive(src, 0.0f, 0.0f);
}

void car_set_calibration(const motors_config_t *cfg) {
    /* Publish a new immutable copy. Readers on the control path see either the old
       pointer or the new one, never a half-written struct. */
    s_cfg_buf[s_cfg_next] = *cfg;
    atomic_store(&s_cfg, &s_cfg_buf[s_cfg_next]);
    s_cfg_next ^= 1;
}

void car_set_trim(int8_t pct) {
    if (pct > 30) pct = 30;
    if (pct < -30) pct = -30;
    atomic_store(&s_trim_pct, pct);
}

int8_t car_get_trim(void) {
    return (int8_t)atomic_load(&s_trim_pct);
}

// JSON string in NVS under "trim": {"trim_pct":..}
void car_save_trim(void) {
    char buf[32];
    snprintf(buf, sizeof(buf), "{\"trim_pct\":%d}", car_get_trim());
    cfg_json_save("trim", buf);
}

bool car_spin_pair(uint8_t pair, bool forward) {
    if (pair > 3) return false;
    motor_outputs_t out = { .duty = {0} };
    const uint16_t duty = 1600;  /* ~40% for identification */
    out.duty[pair * 2]     = forward ? duty : 0;
    out.duty[pair * 2 + 1] = forward ? 0 : duty;
    return link_set(LINK_SRC_CALIB, out.duty, LINK_HOLD_CALIB_MS, false);
}

void car_init(void) {
    motors_config_t loaded;
    if (calibration_load(&loaded)) {
        car_set_calibration(&loaded);
    } else {
        ESP_LOGW(TAG, "no NVS calibration — using default mapping");
    }

    char buf[32];
    if (cfg_json_load("trim", buf, sizeof(buf))) {
        cJSON *j = cJSON_Parse(buf);
        int t;
        if (cfg_json_int(j, "trim_pct", &t) && t >= -30 && t <= 30) car_set_trim((int8_t)t);
        cJSON_Delete(j);
    }

    car_stop(LINK_SRC_SAFE);       /* safety stop */
    link_release(LINK_SRC_SAFE);   /* boot is over; leave the actuator free */
}
