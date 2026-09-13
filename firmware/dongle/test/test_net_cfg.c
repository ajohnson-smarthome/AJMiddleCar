#include "../main/net_cfg.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_accepts_a_normal_network(void) {
    net_cfg_t c;
    assert(net_cfg_validate("SomeNetwork", "secretpass", &c) == NET_CFG_OK);
    assert(strcmp(c.ssid, "SomeNetwork") == 0);
    assert(strcmp(c.password, "secretpass") == 0);
}

static void test_accepts_an_open_network(void) {
    /* An empty password is a valid open network, which the car's identity.h already
       contemplates — not a missing field. */
    net_cfg_t c;
    assert(net_cfg_validate("Open", "", &c) == NET_CFG_OK);
    assert(c.password[0] == '\0');
}

static void test_accepts_a_one_byte_ssid(void) {
    /* The floor of the SSID range (1 byte) was only ever implied by test_ssid_bounds
       rejecting the empty string — assert it's actually accepted, not just "not empty". */
    net_cfg_t c;
    assert(net_cfg_validate("a", "secretpass", &c) == NET_CFG_OK);
    assert(strcmp(c.ssid, "a") == 0);
}

static void test_ssid_bounds(void) {
    net_cfg_t c;
    char max[NET_SSID_MAX + 1];
    memset(max, 'a', NET_SSID_MAX);
    max[NET_SSID_MAX] = '\0';
    assert(net_cfg_validate(max, "secretpass", &c) == NET_CFG_OK);

    char over[NET_SSID_MAX + 2];
    memset(over, 'a', NET_SSID_MAX + 1);
    over[NET_SSID_MAX + 1] = '\0';
    assert(net_cfg_validate(over, "secretpass", &c) == NET_CFG_SSID_LEN);

    assert(net_cfg_validate("", "secretpass", &c) == NET_CFG_SSID_LEN);
}

static void test_ssid_rejects_control_bytes(void) {
    /* net_cfg_render_wifi_reply can only widen a '"' or '\' into two
       bytes, not the six a \uXXXX control-byte escape needs — so what validates must be
       what render can produce, or a downstream buffer sized from the narrower bound
       overruns. 802.11 permits arbitrary octets, but a tab or NUL is not a network
       anyone is trying to reach, unlike a literal quote. */
    net_cfg_t c;
    assert(net_cfg_validate("Net\twork", "secretpass", &c) == NET_CFG_SSID_BYTE);
    assert(net_cfg_validate("Net\x01" "work", "secretpass", &c) == NET_CFG_SSID_BYTE);
    assert(net_cfg_validate("Net\x7f" "work", "secretpass", &c) == NET_CFG_SSID_BYTE);
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

static void test_password_rejects_control_bytes(void) {
    net_cfg_t c;
    assert(net_cfg_validate("net", "pass\t1234", &c) == NET_CFG_PASS_BYTE);
    assert(net_cfg_validate("net", "pass\x01" "1234", &c) == NET_CFG_PASS_BYTE);
    assert(net_cfg_validate("net", "pass\x7f" "1234", &c) == NET_CFG_PASS_BYTE);
}

static void test_a_rejected_body_does_not_write_out(void) {
    /* The caller's current configuration must survive a bad POST intact. */
    net_cfg_t c;
    assert(net_cfg_validate("keep", "secretpass", &c) == NET_CFG_OK);
    assert(net_cfg_validate("", "secretpass", &c) == NET_CFG_SSID_LEN);
    assert(strcmp(c.ssid, "keep") == 0);
}

static void test_errors_name_their_field(void) {
    assert(strcmp(net_cfg_err_field(NET_CFG_SSID_LEN), "ssid") == 0);
    assert(strcmp(net_cfg_err_field(NET_CFG_PASS_LEN), "password") == 0);
    assert(strcmp(net_cfg_err_field(NET_CFG_SSID_BYTE), "ssid") == 0);
    assert(strcmp(net_cfg_err_field(NET_CFG_PASS_BYTE), "password") == 0);
    assert(strcmp(net_cfg_err_field(NET_CFG_OK), "") == 0);
    assert(net_cfg_err_msg(NET_CFG_SSID_LEN)[0] != '\0');
    assert(net_cfg_err_msg(NET_CFG_PASS_LEN)[0] != '\0');
    assert(net_cfg_err_msg(NET_CFG_SSID_BYTE)[0] != '\0');
    assert(net_cfg_err_msg(NET_CFG_PASS_BYTE)[0] != '\0');
}

static void test_errors_name_their_code(void) {
    net_cfg_t c;
    assert(strcmp(net_cfg_err_code(net_cfg_validate("", "drive1234", &c)), DONGLE_ERR_BAD_LENGTH) == 0);
    assert(strcmp(net_cfg_err_code(net_cfg_validate("AJMiddleCar", "short", &c)), DONGLE_ERR_BAD_LENGTH) == 0);
    assert(strcmp(net_cfg_err_code(net_cfg_validate("AJ\tCar", "drive1234", &c)), DONGLE_ERR_BAD_CHARS) == 0);
    assert(strcmp(net_cfg_err_code(net_cfg_validate("AJMiddleCar", "dr\x7fve1234", &c)), DONGLE_ERR_BAD_CHARS) == 0);
    assert(strcmp(net_cfg_err_code(NET_CFG_OK), "") == 0);
}

static void test_wifi_reply_never_leaks_the_password(void) {
    net_cfg_t c;
    assert(net_cfg_validate("AJMiddleCar", "drive1234", &c) == NET_CFG_OK);
    char buf[160];
    int n = net_cfg_render_wifi_reply(&c, DONGLE_WIFI_STATE_SEARCHING, buf, sizeof(buf));
    assert(n > 0 && n == (int)strlen(buf));
    assert(strcmp(buf, "{\"proto\":1,\"ssid\":\"AJMiddleCar\",\"state\":\"searching\"}") == 0);
    assert(strstr(buf, "drive1234") == NULL);
}

static void test_wifi_reply_boundary_is_exact(void) {
    net_cfg_t c;
    assert(net_cfg_validate("AJMiddleCar", "drive1234", &c) == NET_CFG_OK);
    char scratch[160];
    int len = net_cfg_render_wifi_reply(&c, DONGLE_WIFI_STATE_CONNECTED, scratch, sizeof(scratch));
    assert(len > 0);
    assert(net_cfg_render_wifi_reply(&c, DONGLE_WIFI_STATE_CONNECTED, scratch, (size_t)len) == -1);
    assert(net_cfg_render_wifi_reply(&c, DONGLE_WIFI_STATE_CONNECTED, scratch, (size_t)len + 1) == len);
}

static void test_wifi_reply_escapes_a_quote_and_a_backslash(void) {
    net_cfg_t c;
    assert(net_cfg_validate("Say \"hi\"\\", "drive1234", &c) == NET_CFG_OK);
    char buf[200];
    assert(net_cfg_render_wifi_reply(&c, DONGLE_WIFI_STATE_IDLE, buf, sizeof(buf)) > 0);
    assert(strstr(buf, "\"ssid\":\"Say \\\"hi\\\"\\\\\"") != NULL);
}

static void test_validated_values_always_fit_the_reply_worst_case(void) {
    /* Whatever net_cfg_validate accepts must render: 32 bytes of '"' double to 64. */
    char ssid[NET_SSID_MAX + 1];
    memset(ssid, '"', NET_SSID_MAX);
    ssid[NET_SSID_MAX] = '\0';
    net_cfg_t c;
    assert(net_cfg_validate(ssid, "", &c) == NET_CFG_OK);
    char buf[128];   /* 11 + 64 + 2 + 22 (state key + longest word "searching") + NUL, with margin */
    assert(net_cfg_render_wifi_reply(&c, DONGLE_WIFI_STATE_SEARCHING, buf, sizeof(buf)) > 0);
}

static void test_equal_tells_a_retry_from_a_new_network(void) {
    net_cfg_t a, b;
    assert(net_cfg_validate("net", "secretpass", &a) == NET_CFG_OK);
    assert(net_cfg_validate("net", "secretpass", &b) == NET_CFG_OK);
    assert(net_cfg_equal(&a, &b));
    assert(net_cfg_validate("net", "otherpass2", &b) == NET_CFG_OK);
    assert(!net_cfg_equal(&a, &b));
    assert(net_cfg_validate("other", "secretpass", &b) == NET_CFG_OK);
    assert(!net_cfg_equal(&a, &b));
}

/* net_cfg_escape exists so /status can escape a single field (the SSID) into a body
 * net_cfg does not own, without growing a second escaper that could drift from the one
 * the whole-object renders above use. It must go through the same append_escaped they do
 * — these tests exercise it standalone rather than through a render, but the escaping
 * behaviour itself is already pinned by test_wifi_reply_escapes_a_quote_and_a_backslash
 * above. */
static void test_escape_passes_plain_text_through_unchanged(void) {
    char buf[16];
    int n = net_cfg_escape("hello", buf, sizeof(buf));
    assert(n == 5);
    assert(strcmp(buf, "hello") == 0);
}

static void test_escape_refuses_a_buffer_one_byte_too_small(void) {
    /* "ab" needs 2 bytes of content plus a NUL — 3 bytes minimum. One short of that
       must refuse exactly like append_str's per-chunk check the renders rely on. */
    char buf[8];
    assert(net_cfg_escape("ab", buf, 2) == -1);
}

static void test_escape_succeeds_in_a_buffer_exactly_large_enough(void) {
    char buf[8];
    assert(net_cfg_escape("ab", buf, 3) == 2);
    assert(strcmp(buf, "ab") == 0);
}

static void test_escape_refuses_a_zero_length_buffer(void) {
    /* Nothing fits in zero bytes, not even an empty string's own NUL. Without the guard
       in net_cfg_escape, an empty `in` skips the escaping loop entirely and falls
       straight through to a write at out[0] — on a buffer with no bytes to write into. */
    char buf[4] = { 'X', 'X', 'X', 'X' };
    assert(net_cfg_escape("", buf, 0) == -1);
    assert(buf[0] == 'X'); /* untouched, not just "refused" */
}

int main(void) {
    test_accepts_a_normal_network();
    test_accepts_an_open_network();
    test_accepts_a_one_byte_ssid();
    test_ssid_bounds();
    test_ssid_rejects_control_bytes();
    test_password_bounds();
    test_password_rejects_control_bytes();
    test_a_rejected_body_does_not_write_out();
    test_errors_name_their_field();
    test_errors_name_their_code();
    test_wifi_reply_never_leaks_the_password();
    test_wifi_reply_boundary_is_exact();
    test_wifi_reply_escapes_a_quote_and_a_backslash();
    test_equal_tells_a_retry_from_a_new_network();
    test_validated_values_always_fit_the_reply_worst_case();
    test_escape_passes_plain_text_through_unchanged();
    test_escape_refuses_a_buffer_one_byte_too_small();
    test_escape_succeeds_in_a_buffer_exactly_large_enough();
    test_escape_refuses_a_zero_length_buffer();
    printf("test_net_cfg: all passed\n");
    return 0;
}
