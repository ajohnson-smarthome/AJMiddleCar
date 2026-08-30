#include "../main/net_cfg.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_accepts_a_normal_network(void) {
    net_cfg_t c;
    assert(net_cfg_validate("AJMiddleCar", "drive1234", &c) == NET_CFG_OK);
    assert(strcmp(c.ssid, "AJMiddleCar") == 0);
    assert(strcmp(c.password, "drive1234") == 0);
}

static void test_accepts_an_open_network(void) {
    /* An empty password is a valid open network, which the car's identity.h already
       contemplates — not a missing field. */
    net_cfg_t c;
    assert(net_cfg_validate("Open", "", &c) == NET_CFG_OK);
    assert(c.password[0] == '\0');
}

static void test_ssid_bounds(void) {
    net_cfg_t c;
    char max[NET_SSID_MAX + 1];
    memset(max, 'a', NET_SSID_MAX);
    max[NET_SSID_MAX] = '\0';
    assert(net_cfg_validate(max, "drive1234", &c) == NET_CFG_OK);

    char over[NET_SSID_MAX + 2];
    memset(over, 'a', NET_SSID_MAX + 1);
    over[NET_SSID_MAX + 1] = '\0';
    assert(net_cfg_validate(over, "drive1234", &c) == NET_CFG_SSID_LEN);

    assert(net_cfg_validate("", "drive1234", &c) == NET_CFG_SSID_LEN);
}

static void test_password_bounds(void) {
    net_cfg_t c;
    assert(net_cfg_validate("net", "1234567", &c) == NET_CFG_PASS_LEN);   /* 7, one short */
    assert(net_cfg_validate("net", "12345678", &c) == NET_CFG_OK);        /* 8, the floor */

    char max[NET_PASS_MAX + 1];
    memset(max, 'p', NET_PASS_MAX);
    max[NET_PASS_MAX] = '\0';
    assert(net_cfg_validate("net", max, &c) == NET_CFG_OK);

    char over[NET_PASS_MAX + 2];
    memset(over, 'p', NET_PASS_MAX + 1);
    over[NET_PASS_MAX + 1] = '\0';
    assert(net_cfg_validate("net", over, &c) == NET_CFG_PASS_LEN);
}

static void test_a_rejected_body_does_not_write_out(void) {
    /* The caller's stored configuration must survive a bad POST intact. */
    net_cfg_t c;
    assert(net_cfg_validate("keep", "drive1234", &c) == NET_CFG_OK);
    assert(net_cfg_validate("", "drive1234", &c) == NET_CFG_SSID_LEN);
    assert(strcmp(c.ssid, "keep") == 0);
}

static void test_errors_name_their_field(void) {
    assert(strcmp(net_cfg_err_field(NET_CFG_SSID_LEN), "ssid") == 0);
    assert(strcmp(net_cfg_err_field(NET_CFG_PASS_LEN), "password") == 0);
    assert(strcmp(net_cfg_err_field(NET_CFG_OK), "") == 0);
    assert(net_cfg_err_msg(NET_CFG_SSID_LEN)[0] != '\0');
    assert(net_cfg_err_msg(NET_CFG_PASS_LEN)[0] != '\0');
}

static void test_public_render_never_leaks_the_password(void) {
    net_cfg_t c;
    assert(net_cfg_validate("AJMiddleCar", "drive1234", &c) == NET_CFG_OK);

    char buf[128];
    int n = net_cfg_render_public(&c, true, buf, sizeof(buf));
    assert(n > 0 && (size_t)n == strlen(buf));
    assert(strstr(buf, "drive1234") == NULL);
    assert(strstr(buf, "\"ssid\":\"AJMiddleCar\"") != NULL);
    assert(strstr(buf, "\"configured\":true") != NULL);
}

static void test_public_render_when_unconfigured(void) {
    net_cfg_t c = { .ssid = "", .password = "" };
    char buf[128];
    assert(net_cfg_render_public(&c, false, buf, sizeof(buf)) > 0);
    assert(strstr(buf, "\"ssid\":\"\"") != NULL);
    assert(strstr(buf, "\"configured\":false") != NULL);
}

static void test_stored_render_round_trips(void) {
    net_cfg_t c;
    assert(net_cfg_validate("AJMiddleCar", "drive1234", &c) == NET_CFG_OK);
    char buf[192];
    assert(net_cfg_render_stored(&c, buf, sizeof(buf)) > 0);
    /* The stored form is what the dongle reloads to rejoin unaided, so it must carry
       the password that the public form must not. */
    assert(strstr(buf, "drive1234") != NULL);
    assert(strstr(buf, "AJMiddleCar") != NULL);
}

static void test_render_refuses_a_small_buffer(void) {
    net_cfg_t c;
    assert(net_cfg_validate("AJMiddleCar", "drive1234", &c) == NET_CFG_OK);
    char tiny[8];
    assert(net_cfg_render_public(&c, true, tiny, sizeof(tiny)) == -1);
    assert(net_cfg_render_stored(&c, tiny, sizeof(tiny)) == -1);
}

static void test_equal_drives_the_dirty_check(void) {
    net_cfg_t a, b;
    assert(net_cfg_validate("net", "drive1234", &a) == NET_CFG_OK);
    assert(net_cfg_validate("net", "drive1234", &b) == NET_CFG_OK);
    assert(net_cfg_equal(&a, &b));
    assert(net_cfg_validate("net", "drive9999", &b) == NET_CFG_OK);
    assert(!net_cfg_equal(&a, &b));
    assert(net_cfg_validate("other", "drive1234", &b) == NET_CFG_OK);
    assert(!net_cfg_equal(&a, &b));
}

int main(void) {
    test_accepts_a_normal_network();
    test_accepts_an_open_network();
    test_ssid_bounds();
    test_password_bounds();
    test_a_rejected_body_does_not_write_out();
    test_errors_name_their_field();
    test_public_render_never_leaks_the_password();
    test_public_render_when_unconfigured();
    test_stored_render_round_trips();
    test_render_refuses_a_small_buffer();
    test_equal_drives_the_dirty_check();
    printf("test_net_cfg: all passed\n");
    return 0;
}
