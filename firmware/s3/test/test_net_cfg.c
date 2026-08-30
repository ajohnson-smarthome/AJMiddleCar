#include "../main/net_cfg.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* "Big enough" scratch buffers for the tests that aren't specifically pinning the
 * truncation boundary. Sized from the render templates' literal bytes plus the worst
 * realistic expansion — a '"' or '\' in the SSID/password doubles to two bytes; nothing
 * below ever puts a control byte (which escapes to six bytes, \u00XX) at NET_SSID_MAX or
 * NET_PASS_MAX length, so 2x per byte is enough headroom for every case here.
 *   public: 25 literal + 32*2 (ssid)               + 5 ("false")  = 94, +1 NUL = 95
 *   stored: 25 literal + 32*2 (ssid) + 63*2 (password)            = 215, +1 NUL = 216 */
#define PUBLIC_BUF_MAX 95
#define STORED_BUF_MAX 216

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

static void test_accepts_a_one_byte_ssid(void) {
    /* The floor of the SSID range (1 byte) was only ever implied by test_ssid_bounds
       rejecting the empty string — assert it's actually accepted, not just "not empty". */
    net_cfg_t c;
    assert(net_cfg_validate("a", "drive1234", &c) == NET_CFG_OK);
    assert(strcmp(c.ssid, "a") == 0);
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

    char buf[PUBLIC_BUF_MAX];
    int n = net_cfg_render_public(&c, true, buf, sizeof(buf));
    assert(n > 0 && (size_t)n == strlen(buf));
    assert(strstr(buf, "drive1234") == NULL);
    assert(strstr(buf, "\"ssid\":\"AJMiddleCar\"") != NULL);
    assert(strstr(buf, "\"configured\":true") != NULL);
}

static void test_public_render_when_unconfigured(void) {
    net_cfg_t c = { .ssid = "", .password = "" };
    char buf[PUBLIC_BUF_MAX];
    assert(net_cfg_render_public(&c, false, buf, sizeof(buf)) > 0);
    assert(strstr(buf, "\"ssid\":\"\"") != NULL);
    assert(strstr(buf, "\"configured\":false") != NULL);
}

static void test_stored_render_round_trips(void) {
    net_cfg_t c;
    assert(net_cfg_validate("AJMiddleCar", "drive1234", &c) == NET_CFG_OK);
    char buf[STORED_BUF_MAX];
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

static void test_public_render_boundary_is_exact(void) {
    /* A regression from `(size_t)w >= n` to `> n` would accept a body one byte short of
       room for its NUL and pass every other render test here, since all of them use a
       buffer far above the worst case. Pin the exact boundary instead of trusting size. */
    net_cfg_t c;
    assert(net_cfg_validate("AJMiddleCar", "drive1234", &c) == NET_CFG_OK);
    char scratch[PUBLIC_BUF_MAX];
    int len = net_cfg_render_public(&c, true, scratch, sizeof(scratch));
    assert(len > 0);
    assert(net_cfg_render_public(&c, true, scratch, (size_t)len) == -1);
    assert(net_cfg_render_public(&c, true, scratch, (size_t)len + 1) == len);
}

static void test_stored_render_boundary_is_exact(void) {
    net_cfg_t c;
    assert(net_cfg_validate("AJMiddleCar", "drive1234", &c) == NET_CFG_OK);
    char scratch[STORED_BUF_MAX];
    int len = net_cfg_render_stored(&c, scratch, sizeof(scratch));
    assert(len > 0);
    assert(net_cfg_render_stored(&c, scratch, (size_t)len) == -1);
    assert(net_cfg_render_stored(&c, scratch, (size_t)len + 1) == len);
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

static void test_max_length_ssid_and_password_render_together(void) {
    net_cfg_t c;
    char ssid[NET_SSID_MAX + 1];
    char pass[NET_PASS_MAX + 1];
    memset(ssid, 'a', NET_SSID_MAX);
    ssid[NET_SSID_MAX] = '\0';
    memset(pass, 'p', NET_PASS_MAX);
    pass[NET_PASS_MAX] = '\0';
    assert(net_cfg_validate(ssid, pass, &c) == NET_CFG_OK);

    char buf[STORED_BUF_MAX];
    assert(net_cfg_render_stored(&c, buf, sizeof(buf)) > 0);
    assert(strstr(buf, ssid) != NULL);
    assert(strstr(buf, pass) != NULL);
}

static void test_render_escapes_a_quote_in_the_ssid(void) {
    net_cfg_t c;
    assert(net_cfg_validate("Net\"work", "drive1234", &c) == NET_CFG_OK);

    char pub[PUBLIC_BUF_MAX];
    assert(net_cfg_render_public(&c, true, pub, sizeof(pub)) > 0);
    assert(strstr(pub, "\"ssid\":\"Net\\\"work\"") != NULL);

    char stored[STORED_BUF_MAX];
    assert(net_cfg_render_stored(&c, stored, sizeof(stored)) > 0);
    assert(strstr(stored, "\"ssid\":\"Net\\\"work\"") != NULL);
}

static void test_render_escapes_a_backslash_in_the_ssid(void) {
    net_cfg_t c;
    assert(net_cfg_validate("Net\\work", "drive1234", &c) == NET_CFG_OK);

    char pub[PUBLIC_BUF_MAX];
    assert(net_cfg_render_public(&c, true, pub, sizeof(pub)) > 0);
    assert(strstr(pub, "\"ssid\":\"Net\\\\work\"") != NULL);

    char stored[STORED_BUF_MAX];
    assert(net_cfg_render_stored(&c, stored, sizeof(stored)) > 0);
    assert(strstr(stored, "\"ssid\":\"Net\\\\work\"") != NULL);
}

static void test_render_escapes_a_control_byte_in_the_ssid(void) {
    net_cfg_t c;
    assert(net_cfg_validate("Net\x01work", "drive1234", &c) == NET_CFG_OK);

    char pub[PUBLIC_BUF_MAX];
    assert(net_cfg_render_public(&c, true, pub, sizeof(pub)) > 0);
    assert(strstr(pub, "\"ssid\":\"Net\\u0001work\"") != NULL);

    char stored[STORED_BUF_MAX];
    assert(net_cfg_render_stored(&c, stored, sizeof(stored)) > 0);
    assert(strstr(stored, "\"ssid\":\"Net\\u0001work\"") != NULL);
}

static void test_stored_render_escapes_a_quoted_ssid_exactly(void) {
    /* The point is that the output is parseable — assert the exact escaped bytes rather
       than merely that the raw quote is gone. */
    net_cfg_t c;
    assert(net_cfg_validate("Net\"work", "drive1234", &c) == NET_CFG_OK);
    char buf[STORED_BUF_MAX];
    assert(net_cfg_render_stored(&c, buf, sizeof(buf)) > 0);
    assert(strcmp(buf, "{\"ssid\":\"Net\\\"work\",\"password\":\"drive1234\"}") == 0);
}

static void test_stored_render_escapes_a_quoted_password_exactly(void) {
    /* Same property, same code path (append_escaped), the other field. */
    net_cfg_t c;
    assert(net_cfg_validate("net", "pass\"word1", &c) == NET_CFG_OK);
    char buf[STORED_BUF_MAX];
    assert(net_cfg_render_stored(&c, buf, sizeof(buf)) > 0);
    assert(strcmp(buf, "{\"ssid\":\"net\",\"password\":\"pass\\\"word1\"}") == 0);
}

int main(void) {
    test_accepts_a_normal_network();
    test_accepts_an_open_network();
    test_accepts_a_one_byte_ssid();
    test_ssid_bounds();
    test_password_bounds();
    test_a_rejected_body_does_not_write_out();
    test_errors_name_their_field();
    test_public_render_never_leaks_the_password();
    test_public_render_when_unconfigured();
    test_stored_render_round_trips();
    test_render_refuses_a_small_buffer();
    test_public_render_boundary_is_exact();
    test_stored_render_boundary_is_exact();
    test_equal_drives_the_dirty_check();
    test_max_length_ssid_and_password_render_together();
    test_render_escapes_a_quote_in_the_ssid();
    test_render_escapes_a_backslash_in_the_ssid();
    test_render_escapes_a_control_byte_in_the_ssid();
    test_stored_render_escapes_a_quoted_ssid_exactly();
    test_stored_render_escapes_a_quoted_password_exactly();
    printf("test_net_cfg: all passed\n");
    return 0;
}
