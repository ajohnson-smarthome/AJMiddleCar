#ifndef NET_CFG_H
#define NET_CFG_H

#include <stdbool.h>
#include <stddef.h>

/* The network the dongle has been told to join.
 *
 * Pure: no ESP-IDF, no cJSON, no NVS, so the rules below are host-tested with plain `cc`
 * rather than reasoned about. net_api.c does the JSON extraction and the flash writing
 * around this module and holds no rules of its own.
 *
 * The SSID is an opaque string. This firmware does not know what a car is and must not
 * learn: the value arrives over the wire and is stored and replayed unread. */

/* WPA2's limits, not ours. 32 bytes is the maximum SSID length; a PSK shorter than 8
 * characters cannot be used, and an empty password means an open network. */
#define NET_SSID_MAX   32
#define NET_PASS_MIN    8
#define NET_PASS_MAX   63

typedef struct {
    char ssid[NET_SSID_MAX + 1];
    char password[NET_PASS_MAX + 1];
} net_cfg_t;

typedef enum {
    NET_CFG_OK = 0,
    NET_CFG_SSID_LEN,
    NET_CFG_PASS_LEN,
} net_cfg_err_t;

/* Validate and copy. `*out` is written only on NET_CFG_OK; a rejected body leaves the
 * caller's stored configuration untouched, which is why validation happens before any
 * flash write rather than during it.
 *
 * Values are rejected, never clamped — the car's domains behave the same way, and a
 * silently truncated SSID would fail to associate with no visible cause. */
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
 * Returns the length written, or -1 if buf is too small. */
int net_cfg_render_public(const net_cfg_t *cfg, bool configured, char *buf, size_t n);

/* The NVS body. Contains the password — it has to, since this is what the dongle reloads
 * at boot to rejoin without being told again. Returns the length written, or -1. */
int net_cfg_render_stored(const net_cfg_t *cfg, char *buf, size_t n);

/* Whether two configurations are the same, for the dirty check that keeps an unchanged
 * POST from rewriting flash. */
bool net_cfg_equal(const net_cfg_t *a, const net_cfg_t *b);

#endif /* NET_CFG_H */
