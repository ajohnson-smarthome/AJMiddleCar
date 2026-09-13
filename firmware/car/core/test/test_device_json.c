#include "../main/device_json.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    assert(fw_build_number("v1.0+784") == 784);
    assert(fw_build_number("v1.0+784-dirty") == 784);
    assert(fw_build_number("v1.0") == -1);
    assert(fw_build_number("v1.0+") == -1);
    assert(fw_build_number("v1.0+x") == -1);
    assert(fw_build_number("") == -1);

    char buf[160];
    int n = device_group_json(buf, sizeof(buf), "v1.0+784", false);
    assert(n > 0 && n == (int)strlen(buf));
    assert(strcmp(buf, "\"device\":{\"id\":\"ajmiddlecar\",\"fw\":\"v1.0+784\",\"build\":784,"
                       "\"rolled_back\":false}") == 0);
    n = device_group_json(buf, sizeof(buf), "v1.0", true);
    assert(n > 0 && strstr(buf, "\"build\":-1") && strstr(buf, "\"rolled_back\":true"));
    assert(device_group_json(buf, 20, "v1.0+784", false) == -1);
    printf("test_device_json: all passed\n");
    return 0;
}
