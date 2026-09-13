#include "../main/calib_wire.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    assert(calib_corner_index("front_left") == POS_FL);
    assert(calib_corner_index("rear_right") == POS_RR);
    assert(calib_corner_index("Front_Left") == -1);
    assert(calib_corner_index("") == -1);
    assert(strcmp(calib_corner_word(POS_RL), "rear_left") == 0);
    assert(calib_corner_word(7) == NULL);
    assert(calib_direction_forward("forward") == 1);
    assert(calib_direction_forward("reverse") == 0);
    assert(calib_direction_forward("fwd") == -1);

    motors_config_t cfg = { .deadzone = 0.05f };
    cfg.wheels[POS_FL] = (wheel_calib_t){ .channel_pair = 0, .sign = 1 };
    cfg.wheels[POS_FR] = (wheel_calib_t){ .channel_pair = 1, .sign = 1 };
    cfg.wheels[POS_RL] = (wheel_calib_t){ .channel_pair = 2, .sign = -1 };
    cfg.wheels[POS_RR] = (wheel_calib_t){ .channel_pair = 3, .sign = 1 };
    char buf[320];
    int n = calib_table_json(buf, sizeof(buf), true, &cfg);
    assert(n > 0 && n == (int)strlen(buf));
    assert(strcmp(buf,
        "\"calibrated\":true,\"wheels\":["
        "{\"corner\":\"front_left\",\"pair\":0,\"inverted\":false},"
        "{\"corner\":\"front_right\",\"pair\":1,\"inverted\":false},"
        "{\"corner\":\"rear_left\",\"pair\":2,\"inverted\":true},"
        "{\"corner\":\"rear_right\",\"pair\":3,\"inverted\":false}]") == 0);
    n = calib_table_json(buf, sizeof(buf), false, &cfg);
    assert(n > 0 && strcmp(buf, "\"calibrated\":false,\"wheels\":[]") == 0);
    assert(calib_table_json(buf, 40, true, &cfg) == -1);
    printf("test_calib_wire: all passed\n");
    return 0;
}
