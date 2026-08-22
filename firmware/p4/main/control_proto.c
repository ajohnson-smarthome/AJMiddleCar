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

/* Point *val at the value for `key`, which must sit in key position at brace depth 1 —
   a key inside a nested object or inside a string value cannot match. Returns 0 and
   fills *val and *left when the key appears exactly once; 1 when it is absent; -1 when it
   appears twice, because a duplicated key is two instructions in one datagram and the
   car must not pick one (the mock's json.loads is made to refuse the same bytes). */
static int value_of(const char *msg, size_t len, const char *key,
                    const char **val, size_t *left) {
    size_t klen = strlen(key);
    int depth = 0;
    bool in_str = false;
    const char *found = NULL;
    for (size_t i = 0; i < len; i++) {
        char c = msg[i];
        if (in_str) {
            if (c == '\\') { i++; continue; }   /* skip the escaped character */
            if (c == '"') in_str = false;
            continue;
        }
        if (c == '{') { depth++; continue; }
        if (c == '}') { depth--; continue; }
        if (c != '"') continue;
        /* A string opens at i. Is it `key`, in key position, at the top level? Depth
           and a trailing colon are not enough on their own: a *value* that happens to
           spell the key's name, immediately followed by a colon that is itself part of
           malformed JSON (e.g. "note":"t":0), would otherwise be misread as the key —
           json.loads never reads a value as a key no matter what follows it, so this
           checks the byte immediately before the quote too, the way a real key is
           always introduced: by '{' or ','. */
        if (depth == 1 && i + klen + 1 < len &&
            memcmp(msg + i + 1, key, klen) == 0 && msg[i + 1 + klen] == '"') {
            size_t b = i;
            while (b > 0 && is_ws(msg[b - 1])) b--;
            if (b > 0 && (msg[b - 1] == '{' || msg[b - 1] == ',')) {
                size_t q = i + klen + 2;
                while (q < len && is_ws(msg[q])) q++;
                if (q < len && msg[q] == ':') {
                    q++;
                    while (q < len && is_ws(msg[q])) q++;
                    if (q >= len) return -1;         /* key with nothing after the colon */
                    if (found != NULL) return -1;    /* second occurrence */
                    found = msg + q;
                    *left = len - q;
                    i = q - 1;                       /* resume scanning at the value */
                    continue;
                }
            }
        }
        in_str = true;                           /* an ordinary string: skip its body */
    }
    if (found == NULL) return 1;
    *val = found;
    return 0;
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

/* JSON's number grammar and nothing more. strtof/strtoull are laxer than the wire:
   they took ".5", "+1", "0123" and "1.5"-truncated-to-1, and the mock's json.loads
   refused every one of them — byte-identical datagrams drove the two cars apart. */
static bool token_ends(const char *p, size_t n, size_t k) {
    if (k >= n) return true;
    char c = p[k];
    return is_ws(c) || c == ',' || c == '}';
}

static bool json_int_shape(const char *s) {
    size_t i = (s[0] == '-') ? 1u : 0u;
    if (s[i] == '\0') return false;
    if (s[i] == '0' && s[i + 1] != '\0') return false;   /* leading zero */
    for (; s[i] != '\0'; i++) if (s[i] < '0' || s[i] > '9') return false;
    return true;
}

static bool json_num_shape(const char *s) {
    size_t i = (s[0] == '-') ? 1u : 0u;
    if (s[i] < '0' || s[i] > '9') return false;          /* "+1", ".5", "-", "" */
    if (s[i] == '0' && s[i + 1] != '\0' &&
        s[i + 1] != '.' && s[i + 1] != 'e' && s[i + 1] != 'E') return false;
    i++;
    while (s[i] >= '0' && s[i] <= '9') i++;
    if (s[i] == '.') {
        i++;
        if (s[i] < '0' || s[i] > '9') return false;
        while (s[i] >= '0' && s[i] <= '9') i++;
    }
    if (s[i] == 'e' || s[i] == 'E') {
        i++;
        if (s[i] == '+' || s[i] == '-') i++;
        if (s[i] < '0' || s[i] > '9') return false;
        while (s[i] >= '0' && s[i] <= '9') i++;
    }
    return s[i] == '\0';
}

static int parse_num(const char *p, size_t n, float *out) {
    char tmp[24];
    int k = token(p, n, "0123456789+-.eE", tmp, sizeof(tmp));
    if (k < 0) return -1;
    if (!token_ends(p, n, (size_t)k) || !json_num_shape(tmp)) return -1;
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
    /* A u32 (seq, proto) must be the whole value: parse_u32's digit-only token used to
       stop at the '.' of "1.5" and accept the truncation, so a fractional proto was
       adopted as version 1. */
    if (!token_ends(p, n, (size_t)k) || !json_int_shape(tmp)) return -1;
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
    const char *v = NULL;
    size_t left = 0;   /* every reader below is guarded by its lookup, but an
                          indeterminate read is not something to leave lying around */

    int r;
    r = value_of(msg, len, RT_KEY_PROTO, &v, &left);
    if (r < 0) return -1;
    if (r == 0) {
        if (parse_u32(v, left, &f.proto) != 0) return -1;
        f.has_proto = true;
    }
    r = value_of(msg, len, RT_KEY_SEQ, &v, &left);
    if (r < 0) return -1;
    if (r == 0) {
        if (parse_u32(v, left, &f.seq) != 0) return -1;
        f.has_seq = true;
    }
    r = value_of(msg, len, RT_KEY_HELLO, &v, &left);
    if (r < 0) return -1;
    if (r == 0) {
        if (parse_sid(v, left, f.sid, sizeof(f.sid)) != 0) return -1;
        f.has_hello = true;
    }
    r = value_of(msg, len, RT_KEY_BYE, &v, &left);
    if (r < 0) return -1;
    if (r == 0) {
        /* The wire says 1, but JSON has two ways to say yes and a client that picks the
           other one must not have its goodbye read as a drive command. */
        float b;
        if (left >= 4 && memcmp(v, "true", 4) == 0 && token_ends(v, left, 4)) f.bye = true;
        else if (left >= 5 && memcmp(v, "false", 5) == 0 && token_ends(v, left, 5)) f.bye = false;
        else if (parse_num(v, left, &b) == 0) f.bye = (b != 0.0f);
        else return -1;
    }

    const char *vt = NULL, *vy = NULL;
    size_t left_t = 0, left_y = 0;
    r = value_of(msg, len, RT_KEY_THROTTLE, &vt, &left_t);
    if (r < 0) return -1;
    int ry = value_of(msg, len, RT_KEY_YAW, &vy, &left_y);
    if (ry < 0) return -1;
    if (vt != NULL || vy != NULL) {
        /* One axis without the other is a truncated or corrupt frame, not a command to
           hold the missing axis at zero. */
        if (vt == NULL || vy == NULL) return -1;
        if (parse_num(vt, left_t, &f.t) != 0) return -1;
        if (parse_num(vy, left_y, &f.y) != 0) return -1;
        f.has_ty = true;
    }

    /* Something to act on: a hello, a goodbye, or both axes. A goodbye counts without
       the axes it usually carries — they are zeroes the stop would write anyway. */
    if (!f.has_hello && !f.has_ty && !f.bye) return -1;
    /* And something the transport can order. Every app->car datagram except a hello
       carries seq — a goodbye included — because one without it would bypass replay
       protection, so the whole frame is dropped rather than half-honoured. The rule
       lives here as well as in rt_link so that the two halves cannot disagree about
       which datagrams the car acts on: they did, and a goodbye the parser accepted and
       the transport dropped looked like a working feature. */
    if (!f.has_hello && !f.has_seq) return -1;
    *out = f;
    return 0;
}
