#include "../main/net_cfg.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* "Big enough" scratch buffers for the tests that aren't specifically pinning the
 * truncation boundary. net_cfg_validate rejects control bytes and DEL outright (see
 * test_ssid_rejects_control_bytes / test_password_rejects_control_bytes below), so the
 * only escape a *validated* value can still trigger is '"' or '\' doubling to two bytes —
 * the six-byte \uXXXX case in net_cfg.c's append_escaped is defence for a value that
 * reached render_* some other way than net_cfg_validate (net_cfg_t's fields are plain, so
 * a caller can bypass it; test_render_still_escapes_a_legacy_control_byte does exactly
 * that) and these buffers are not sized for it.
 *   public: 25 literal + 32*2 (ssid)               + 5 ("false")  = 94, +1 NUL = 95
 *   stored: 25 literal + 32*2 (ssid) + 63*2 (password)            = 215, +1 NUL = 216
 * test_validated_values_always_fit_the_stored_worst_case and its public-render sibling
 * are what pin these as a proven bound rather than a hopeful one — that pair is what
 * would have caught the bug where these numbers were first wrong. */
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

static void test_ssid_rejects_control_bytes(void) {
    /* net_cfg_render_public/net_cfg_render_stored can only widen a '"' or '\' into two
       bytes, not the six a \uXXXX control-byte escape needs — so what validates must be
       what render can produce, or a downstream buffer sized from the narrower bound
       overruns. 802.11 permits arbitrary octets, but a tab or NUL is not a network
       anyone is trying to reach, unlike a literal quote. */
    net_cfg_t c;
    assert(net_cfg_validate("Net\twork", "drive1234", &c) == NET_CFG_SSID_BYTE);
    assert(net_cfg_validate("Net\x01" "work", "drive1234", &c) == NET_CFG_SSID_BYTE);
    assert(net_cfg_validate("Net\x7f" "work", "drive1234", &c) == NET_CFG_SSID_BYTE);
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
    /* The caller's stored configuration must survive a bad POST intact. */
    net_cfg_t c;
    assert(net_cfg_validate("keep", "drive1234", &c) == NET_CFG_OK);
    assert(net_cfg_validate("", "drive1234", &c) == NET_CFG_SSID_LEN);
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

static void test_validated_values_always_fit_the_public_worst_case(void) {
    /* The invariant that matters: whatever net_cfg_validate accepts, net_cfg_render_public
       must fit into PUBLIC_BUF_MAX. The worst case reachable now that control bytes are
       refused is every SSID byte at max length being a '"', the only escape that still
       expands — this is the case that would have caught the original bug, where the
       buffer math assumed doubling but validation still let a 6x-expanding SSID through. */
    net_cfg_t c;
    char ssid[NET_SSID_MAX + 1];
    memset(ssid, '"', NET_SSID_MAX);
    ssid[NET_SSID_MAX] = '\0';
    assert(net_cfg_validate(ssid, "drive1234", &c) == NET_CFG_OK);

    char buf[PUBLIC_BUF_MAX];
    assert(net_cfg_render_public(&c, true, buf, sizeof(buf)) > 0);
}

static void test_validated_values_always_fit_the_stored_worst_case(void) {
    /* Same invariant, the stored form: worst case is both fields at max length, entirely
       quotes. This is the pair of tests the review specifically called for. */
    net_cfg_t c;
    char ssid[NET_SSID_MAX + 1];
    char pass[NET_PASS_MAX + 1];
    memset(ssid, '"', NET_SSID_MAX);
    ssid[NET_SSID_MAX] = '\0';
    memset(pass, '"', NET_PASS_MAX);
    pass[NET_PASS_MAX] = '\0';
    assert(net_cfg_validate(ssid, pass, &c) == NET_CFG_OK);

    char buf[STORED_BUF_MAX];
    assert(net_cfg_render_stored(&c, buf, sizeof(buf)) > 0);
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

static void test_render_still_escapes_a_legacy_control_byte(void) {
    /* net_cfg_validate now refuses a control byte outright, so this path is reachable
       only by a value that predates the rule — bytes already sitting in NVS from before
       this fix, say. net_cfg_t's fields are plain char arrays, so a value can be built
       directly without going through net_cfg_validate, which is what simulates that.
       The escaper must still turn it into valid JSON rather than compounding the
       problem: see the comment on append_escaped in net_cfg.c. */
    net_cfg_t c;
    strcpy(c.ssid, "Net\x01" "work");
    strcpy(c.password, "drive1234");

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

/* net_cfg_escape exists so /status can escape a single field (the SSID) into a body
 * net_cfg does not own, without growing a second escaper that could drift from the one
 * the whole-object renders above use. It must go through the same append_escaped they do
 * — these tests exercise it standalone rather than through a render, but the escaping
 * behaviour itself is already pinned by test_render_escapes_a_quote_in_the_ssid and its
 * siblings above. */
static void test_escape_passes_plain_text_through_unchanged(void) {
    char buf[16];
    int n = net_cfg_escape("hello", buf, sizeof(buf));
    assert(n == 5);
    assert(strcmp(buf, "hello") == 0);
}

static void test_escape_doubles_a_quote(void) {
    char buf[16];
    int n = net_cfg_escape("a\"b", buf, sizeof(buf));
    assert(n == 4);
    assert(strcmp(buf, "a\\\"b") == 0);
}

static void test_escape_doubles_a_backslash(void) {
    char buf[16];
    int n = net_cfg_escape("a\\b", buf, sizeof(buf));
    assert(n == 4);
    assert(strcmp(buf, "a\\\\b") == 0);
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
    test_public_render_never_leaks_the_password();
    test_public_render_when_unconfigured();
    test_stored_render_round_trips();
    test_render_refuses_a_small_buffer();
    test_public_render_boundary_is_exact();
    test_stored_render_boundary_is_exact();
    test_equal_drives_the_dirty_check();
    test_max_length_ssid_and_password_render_together();
    test_validated_values_always_fit_the_public_worst_case();
    test_validated_values_always_fit_the_stored_worst_case();
    test_render_escapes_a_quote_in_the_ssid();
    test_render_escapes_a_backslash_in_the_ssid();
    test_render_still_escapes_a_legacy_control_byte();
    test_stored_render_escapes_a_quoted_ssid_exactly();
    test_stored_render_escapes_a_quoted_password_exactly();
    test_escape_passes_plain_text_through_unchanged();
    test_escape_doubles_a_quote();
    test_escape_doubles_a_backslash();
    test_escape_refuses_a_buffer_one_byte_too_small();
    test_escape_succeeds_in_a_buffer_exactly_large_enough();
    printf("test_net_cfg: all passed\n");
    return 0;
}
