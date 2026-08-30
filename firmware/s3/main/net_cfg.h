#ifndef NET_CFG_H
#define NET_CFG_H

#include <stdbool.h>
#include <stddef.h>

#include "dongle_contract.inc"

/* The network the dongle has been told to join.
 *
 * Pure: no ESP-IDF, no cJSON, no NVS, so the rules below are host-tested with plain `cc`
 * rather than reasoned about. net_api.c does the JSON extraction and the flash writing
 * around this module and holds no rules of its own.
 *
 * The SSID is an opaque string. This firmware does not know what a car is and must not
 * learn: the value arrives over the wire and is stored and replayed unread. */

/* WPA2's limits, not ours, and now the contract's: the same four numbers reach the app
 * through app/AJMiddleCar/Generated/DongleAPI.swift, so neither side writes them as a
 * literal and check_contract.sh fails a tree where they disagree.
 *
 * The bounds are named here rather than used directly so the rest of this header reads
 * as it did — and so a reader sees at a glance which numbers are contractual. */
#define NET_SSID_MAX   DONGLE_SSID_MAX
#define NET_PASS_MIN   DONGLE_PASS_MIN
#define NET_PASS_MAX   DONGLE_PASS_MAX

typedef struct {
    char ssid[NET_SSID_MAX + 1];
    char password[NET_PASS_MAX + 1];
} net_cfg_t;

typedef enum {
    NET_CFG_OK = 0,
    NET_CFG_SSID_LEN,
    NET_CFG_PASS_LEN,
    NET_CFG_SSID_BYTE,
    NET_CFG_PASS_BYTE,
} net_cfg_err_t;

/* Validate and copy. `*out` is written only on NET_CFG_OK; a rejected body leaves the
 * caller's stored configuration untouched, which is why validation happens before any
 * flash write rather than during it.
 *
 * Values are rejected, never clamped — the car's domains behave the same way, and a
 * silently truncated SSID would fail to associate with no visible cause.
 *
 * Also rejects any byte below 0x20, or 0x7F (DEL), in either field. What validates here
 * must be what net_cfg_render_public/net_cfg_render_stored can produce: those escape a
 * `"` or `\` by doubling it, not by the six bytes a \uXXXX control-byte escape needs, so a
 * control byte that passed on length alone could overrun a buffer sized from the narrower
 * bound. 802.11 permits arbitrary octets in an SSID, but unlike a literal quote — which is
 * a real network's real name — a tab or NUL is not a network anyone is trying to reach,
 * which is why this rejects rather than escapes it.
 *
 * This is the canonical statement of that guarantee. Every worst-case buffer bound built
 * from NET_SSID_MAX/NET_PASS_MAX — the NVS blob in net_api.c, the render buffers in its
 * tests — is arithmetic derived from exactly this refusal (at most a doubling, never a
 * sixfold \uXXXX expansion). If the rule above ever changes, those buffers are wrong until
 * they are re-derived; they point back here rather than restating the derivation. */
net_cfg_err_t net_cfg_validate(const char *ssid, const char *password, net_cfg_t *out);

/* Which field a rejection blames, for the {"error":…,"field":…} reply shape.
 * "" when the body as a whole is at fault. */
const char *net_cfg_err_field(net_cfg_err_t e);

/* The message that accompanies it. */
const char *net_cfg_err_msg(net_cfg_err_t e);

/* The GET /net body. NEVER contains the password: the app holds that value itself and
 * has no use for reading it back, so an endpoint that returns a stored credential would
 * be a liability with nothing on the other side of the trade.
 *
 * The SSID is an 802.11 octet string, not text: net_cfg_validate bounds its length and now
 * rejects control bytes outright, but still lets a raw '"' or '\' through on purpose — a
 * real network can be named with one, and refusing it would make that network permanently
 * unreachable through this dongle — so this JSON-escapes rather than rejects those bytes.
 * Control bytes are escaped here too, not because validation still admits them, but as
 * defence for a net_cfg_t built without going through net_cfg_validate at all (its fields
 * are plain char arrays, so a caller can bypass it — see append_escaped's comment).
 *
 * Returns the length written, or -1 if buf is too small. */
int net_cfg_render_public(const net_cfg_t *cfg, bool configured, char *buf, size_t n);

/* The NVS body. Contains the password — it has to, since this is what the dongle reloads
 * at boot to rejoin without being told again. Both fields are JSON-escaped for the same
 * reason as net_cfg_render_public — an unescaped quote here would corrupt the very blob
 * the boot-time parser reads back, silently dropping the saved credentials.
 * Returns the length written, or -1. */
int net_cfg_render_stored(const net_cfg_t *cfg, char *buf, size_t n);

/* Escape one string as JSON string CONTENT — the bytes that go between the quotes, without
 * them. The whole-object renders above use the same machinery; this exists because /status
 * builds a body this module does not own and must not therefore grow a second escaper.
 *
 * Returns the length written, or -1 if it will not fit. */
int net_cfg_escape(const char *in, char *out, size_t n);

/* Whether two configurations are the same, for the dirty check that keeps an unchanged
 * POST from rewriting flash. */
bool net_cfg_equal(const net_cfg_t *a, const net_cfg_t *b);

#endif /* NET_CFG_H */
