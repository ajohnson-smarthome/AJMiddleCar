#include "net_cfg.h"

#include <stdio.h>
#include <string.h>

net_cfg_err_t net_cfg_validate(const char *ssid, const char *password, net_cfg_t *out)
{
    /* Defensive: the one caller checks cJSON_IsString first, so neither can be NULL in
     * practice. Each blames its own field anyway — an error that names the wrong one
     * costs more to debug than the branch costs to write. */
    if (ssid == NULL) {
        return NET_CFG_SSID_LEN;
    }
    if (password == NULL) {
        return NET_CFG_PASS_LEN;
    }

    size_t sn = strlen(ssid);
    if (sn < DONGLE_SSID_MIN || sn > NET_SSID_MAX) {
        return NET_CFG_SSID_LEN;
    }
    for (size_t i = 0; i < sn; i++) {
        unsigned char b = (unsigned char)ssid[i];
        if (b < 0x20 || b == 0x7F) {
            return NET_CFG_SSID_BYTE;
        }
    }

    size_t pn = strlen(password);
    if (pn != 0 && (pn < NET_PASS_MIN || pn > NET_PASS_MAX)) {
        return NET_CFG_PASS_LEN;
    }
    for (size_t i = 0; i < pn; i++) {
        unsigned char b = (unsigned char)password[i];
        if (b < 0x20 || b == 0x7F) {
            return NET_CFG_PASS_BYTE;
        }
    }

    /* Written last, and only here: a rejected body must leave the caller's stored
     * configuration exactly as it was. */
    memcpy(out->ssid, ssid, sn + 1);
    memcpy(out->password, password, pn + 1);
    return NET_CFG_OK;
}

const char *net_cfg_err_field(net_cfg_err_t e)
{
    switch (e) {
    case NET_CFG_SSID_LEN:
    case NET_CFG_SSID_BYTE: return "ssid";
    case NET_CFG_PASS_LEN:
    case NET_CFG_PASS_BYTE: return "password";
    case NET_CFG_OK:        break;
    }
    return "";
}

const char *net_cfg_err_msg(net_cfg_err_t e)
{
    switch (e) {
    case NET_CFG_SSID_LEN:  return "ssid must be 1..32 bytes";
    case NET_CFG_SSID_BYTE: return "ssid must not contain control bytes";
    case NET_CFG_PASS_LEN:  return "password must be empty or 8..63 bytes";
    case NET_CFG_PASS_BYTE: return "password must not contain control bytes";
    case NET_CFG_OK:        break;
    }
    return "";
}

/* Appends `s` to `buf` at `*pos`, refusing if it would leave no room for the terminating
 * NUL that the caller writes once, after the whole body is built. Every literal chunk and
 * every escaped chunk goes through this, so no single piece — however much the escaper
 * below expands it — can write past `n`. This is the same "refuse rather than truncate"
 * rule snprintf's `(size_t)w >= n` check enforced before escaping existed; it just applies
 * per-chunk now instead of to one whole formatted string. */
static bool append_str(char *buf, size_t n, size_t *pos, const char *s)
{
    size_t len = strlen(s);
    if (*pos + len >= n) {
        return false;
    }
    memcpy(buf + *pos, s, len);
    *pos += len;
    return true;
}

/* Appends the JSON-escaped form of `s`: '"' and '\' doubled, control bytes below 0x20 as
 * \u00XX. Why a validated value only ever hits the doubling case, and why that is what
 * makes a caller's buffer arithmetic provable rather than guessed: net_cfg_validate's
 * comment in net_cfg.h.
 *
 * net_cfg_validate refuses control bytes and DEL outright (see its comment), so the
 * \uXXXX branch below is unreachable for anything that passed validation — do not delete
 * it as dead code. It stays as defence: NVS can hold bytes written before that rule
 * existed, and net_cfg_t's fields are plain enough that a caller could construct one
 * without going through net_cfg_validate at all. Either way, a raw control byte reaching
 * this function must still come out as valid JSON. */
static bool append_escaped(char *buf, size_t n, size_t *pos, const char *s)
{
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        char esc[7]; /* longest case is "\u00XX" = 6 bytes + NUL */
        if (*p == '"') {
            strcpy(esc, "\\\"");
        } else if (*p == '\\') {
            strcpy(esc, "\\\\");
        } else if (*p < 0x20) {
            snprintf(esc, sizeof(esc), "\\u%04x", (unsigned)*p);
        } else {
            esc[0] = (char)*p;
            esc[1] = '\0';
        }
        if (!append_str(buf, n, pos, esc)) {
            return false;
        }
    }
    return true;
}

int net_cfg_render_public(const net_cfg_t *cfg, bool configured, char *buf, size_t n)
{
    size_t pos = 0;
    if (!append_str(buf, n, &pos, "{\"ssid\":\"")) {
        return -1;
    }
    if (!append_escaped(buf, n, &pos, cfg->ssid)) {
        return -1;
    }
    if (!append_str(buf, n, &pos, "\",\"configured\":")) {
        return -1;
    }
    if (!append_str(buf, n, &pos, configured ? "true" : "false")) {
        return -1;
    }
    if (!append_str(buf, n, &pos, "}")) {
        return -1;
    }
    buf[pos] = '\0';
    return (int)pos;
}

int net_cfg_render_stored(const net_cfg_t *cfg, char *buf, size_t n)
{
    size_t pos = 0;
    if (!append_str(buf, n, &pos, "{\"ssid\":\"")) {
        return -1;
    }
    if (!append_escaped(buf, n, &pos, cfg->ssid)) {
        return -1;
    }
    if (!append_str(buf, n, &pos, "\",\"password\":\"")) {
        return -1;
    }
    if (!append_escaped(buf, n, &pos, cfg->password)) {
        return -1;
    }
    if (!append_str(buf, n, &pos, "\"}")) {
        return -1;
    }
    buf[pos] = '\0';
    return (int)pos;
}

int net_cfg_escape(const char *in, char *out, size_t n)
{
    /* Nothing fits in zero bytes, not even an empty string's own NUL. Without this guard
     * an empty `in` skips append_escaped's loop entirely and falls straight through to
     * `out[0] = '\0'` — a write to a buffer with no bytes to write into. */
    if (n == 0) {
        return -1;
    }
    size_t pos = 0;
    if (!append_escaped(out, n, &pos, in)) {
        return -1;
    }
    out[pos] = '\0';
    return (int)pos;
}

bool net_cfg_equal(const net_cfg_t *a, const net_cfg_t *b)
{
    return strcmp(a->ssid, b->ssid) == 0 && strcmp(a->password, b->password) == 0;
}
