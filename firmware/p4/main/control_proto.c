#include "control_proto.h"
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "contract.h"   /* the wire's key names — RT_KEY_*, generated from the schema */

/* The 10 Hz hot path, so no JSON library and no allocation: the datagram is scanned in
   place for the handful of keys the real-time channel defines. Everything below is
   bounded by `len` rather than by a NUL, because what arrives from recvfrom is a length
   and a buffer, and terminating it first would be one more thing to get right. */

static bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

/* Point at the value for `key`, or NULL. The key must sit where a key can sit — right
   after '{' or ',' — so a key name appearing inside a string value cannot match. */
static const char *value_of(const char *msg, size_t len, const char *key, size_t *left) {
    size_t klen = strlen(key);
    for (size_t i = 0; i + klen + 2 <= len; i++) {
        if (msg[i] != '"') continue;
        size_t b = i;
        while (b > 0 && is_ws(msg[b - 1])) b--;
        if (b == 0 || (msg[b - 1] != '{' && msg[b - 1] != ',')) continue;
        if (memcmp(msg + i + 1, key, klen) != 0 || msg[i + 1 + klen] != '"') continue;
        size_t q = i + klen + 2;
        while (q < len && is_ws(msg[q])) q++;
        if (q >= len || msg[q] != ':') continue;
        q++;
        while (q < len && is_ws(msg[q])) q++;
        if (q >= len) return NULL;
        *left = len - q;
        return msg + q;
    }
    return NULL;
}

/* strtof and strtoul both want a terminated string, so the token is copied out first.
   The copy is also the length check: a number that does not fit a small buffer is not a
   number this protocol sends. */
static int token(const char *p, size_t n, const char *allowed, char *out, size_t cap) {
    size_t k = 0;
    /* strchr matches the terminator too, so an embedded NUL has to be refused first. */
    while (k < n && k < cap - 1 && p[k] != '\0' && strchr(allowed, p[k]) != NULL) k++;
    if (k == 0 || k >= cap - 1) return -1;
    memcpy(out, p, k);
    out[k] = '\0';
    return (int)k;
}

static int parse_num(const char *p, size_t n, float *out) {
    char tmp[24];
    int k = token(p, n, "0123456789+-.eE", tmp, sizeof(tmp));
    if (k < 0) return -1;
    char *end;
    float v = strtof(tmp, &end);
    /* end == tmp + k rejects "1.2.3" and "1e"; isfinite rejects a value that overflowed
       to inf. "nan" and "inf" never reach here — their letters are not in the token set. */
    if (end != tmp + k || !isfinite(v)) return -1;
    *out = v;
    return 0;
}

static int parse_u32(const char *p, size_t n, uint32_t *out) {
    char tmp[12];
    int k = token(p, n, "0123456789", tmp, sizeof(tmp));
    if (k < 0) return -1;
    char *end;
    /* strtoull, not strtoul: unsigned long is 64 bits on the host and 32 on riscv32, so
       with strtoul the range check below was dead code on the car — an over-large value
       saturated to ULONG_MAX and was accepted there while this module's own host tests
       rejected it. unsigned long long is 64 bits on both, and `token` admits at most ten
       digits, so the conversion cannot saturate and the magnitude test is the whole
       rule, identically in both places. */
    unsigned long long v = strtoull(tmp, &end, 10);
    if (end != tmp + k || v > 0xFFFFFFFFull) return -1;
    *out = (uint32_t)v;
    return 0;
}

/* The session id is echoed straight back in the hello reply, so it is restricted to
   characters that cannot change the shape of that JSON. A quote or a backslash in an
   id would otherwise let the sender dictate the reply's structure. */
static int parse_sid(const char *p, size_t n, char *out, size_t cap) {
    if (n < 2 || p[0] != '"') return -1;
    size_t k = 1;
    while (k < n && p[k] != '"') {
        if (!isalnum((unsigned char)p[k])) return -1;
        if (k >= cap) return -1;                 /* leaves room for the NUL at out[k-1] */
        out[k - 1] = p[k];
        k++;
    }
    if (k >= n) return -1;   /* unterminated */
    if (k == 1) return -1;   /* empty: nothing to tell one session from the next */
    out[k - 1] = '\0';
    return 0;
}

int control_parse_frame(const char *msg, size_t len, size_t max_len, control_frame_t *out) {
    if (msg == NULL || out == NULL || len == 0 || len > max_len) return -1;

    control_frame_t f = {0};
    const char *v;
    size_t left = 0;   /* every reader below is guarded by its lookup, but an
                          indeterminate read is not something to leave lying around */

    if ((v = value_of(msg, len, RT_KEY_PROTO, &left)) != NULL) {
        if (parse_u32(v, left, &f.proto) != 0) return -1;
        f.has_proto = true;
    }
    if ((v = value_of(msg, len, RT_KEY_SEQ, &left)) != NULL) {
        if (parse_u32(v, left, &f.seq) != 0) return -1;
        f.has_seq = true;
    }
    if ((v = value_of(msg, len, RT_KEY_HELLO, &left)) != NULL) {
        if (parse_sid(v, left, f.sid, sizeof(f.sid)) != 0) return -1;
        f.has_hello = true;
    }
    if ((v = value_of(msg, len, RT_KEY_BYE, &left)) != NULL) {
        /* The wire says 1, but JSON has two ways to say yes and a client that picks the
           other one must not have its goodbye read as a drive command. */
        float b;
        if (left >= 4 && memcmp(v, "true", 4) == 0)       f.bye = true;
        else if (left >= 5 && memcmp(v, "false", 5) == 0) f.bye = false;
        else if (parse_num(v, left, &b) == 0)             f.bye = (b != 0.0f);
        else return -1;
    }

    const char *vt = value_of(msg, len, RT_KEY_THROTTLE, &left);
    size_t left_t = left;
    const char *vy = value_of(msg, len, RT_KEY_YAW, &left);
    if (vt != NULL || vy != NULL) {
        /* One axis without the other is a truncated or corrupt frame, not a command to
           hold the missing axis at zero. */
        if (vt == NULL || vy == NULL) return -1;
        if (parse_num(vt, left_t, &f.t) != 0) return -1;
        if (parse_num(vy, left, &f.y) != 0) return -1;
        f.has_ty = true;
    }

    /* A goodbye counts on its own. The axes it usually carries are zeroes the stop
       would write anyway, and a sender that omits them still means stop — dropping the
       frame here would instead leave the car to notice the silence and retreat, which
       is the one thing the goodbye exists to prevent. */
    if (!f.has_hello && !f.has_ty && !f.bye) return -1;   /* nothing to act on */
    *out = f;
    return 0;
}
