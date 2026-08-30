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
    if (sn < 1 || sn > NET_SSID_MAX) {
        return NET_CFG_SSID_LEN;
    }

    size_t pn = strlen(password);
    if (pn != 0 && (pn < NET_PASS_MIN || pn > NET_PASS_MAX)) {
        return NET_CFG_PASS_LEN;
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
    case NET_CFG_SSID_LEN: return "ssid";
    case NET_CFG_PASS_LEN: return "password";
    case NET_CFG_OK:       break;
    }
    return "";
}

const char *net_cfg_err_msg(net_cfg_err_t e)
{
    switch (e) {
    case NET_CFG_SSID_LEN: return "ssid must be 1..32 bytes";
    case NET_CFG_PASS_LEN: return "password must be empty or 8..63 bytes";
    case NET_CFG_OK:       break;
    }
    return "";
}

int net_cfg_render_public(const net_cfg_t *cfg, bool configured, char *buf, size_t n)
{
    int w = snprintf(buf, n, "{\"ssid\":\"%s\",\"configured\":%s}",
                     cfg->ssid, configured ? "true" : "false");
    /* snprintf returns what it WOULD have written, so this catches truncation as well
     * as failure — a half-written body is worse than a refusal. */
    if (w < 0 || (size_t)w >= n) {
        return -1;
    }
    return w;
}

int net_cfg_render_stored(const net_cfg_t *cfg, char *buf, size_t n)
{
    int w = snprintf(buf, n, "{\"ssid\":\"%s\",\"password\":\"%s\"}",
                     cfg->ssid, cfg->password);
    if (w < 0 || (size_t)w >= n) {
        return -1;
    }
    return w;
}

bool net_cfg_equal(const net_cfg_t *a, const net_cfg_t *b)
{
    return strcmp(a->ssid, b->ssid) == 0 && strcmp(a->password, b->password) == 0;
}
