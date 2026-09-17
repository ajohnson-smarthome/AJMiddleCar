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

    /* GET /version: the one document whose shape never changes. Flat, five keys, in order. */
    char ver[160];
    n = version_json(ver, sizeof(ver), "v1.0+879", false);
    assert(n > 0 && n == (int)strlen(ver));
    assert(strcmp(ver, "{\"device\":\"ajmiddlecar\",\"fw\":\"v1.0+879\",\"build\":879,"
                       "\"proto\":2,\"rolled_back\":false}") == 0);
    n = version_json(ver, sizeof(ver), "v1.0", true);
    assert(n > 0 && strstr(ver, "\"build\":-1,\"proto\":2,\"rolled_back\":true}"));
    assert(version_json(ver, 30, "v1.0+879", false) == -1);
    printf("test_device_json: all passed\n");
    return 0;
}
