#ifndef VIDEO_SUB_H
#define VIDEO_SUB_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "contract.h"        /* RT_PROTO, VIDEO_SUBSCRIBE_TIMEOUT_MS, VIDEO_IDR_MIN_MS */
#include "control_proto.h"   /* control_frame_t, CT_VIEW */
#include "watchdog.h"        /* watchdog_stale */

// The subscription, as arithmetic: who may watch (the rt session's owner, by sid), for
// how long after the last `view`, and how often `key:true` may force an IDR. Pure, like
// rt_session_t, so the rules are pinned by test_video_sub.c rather than by the bench.
typedef struct {
    bool     subscribed;
    uint32_t last_view_ms;
    bool     have_idr_ms;
    uint32_t last_idr_ms;
} video_sub_t;

typedef enum {
    VS_IGNORE = 0,   // not a view, a foreign proto, or not the owner's sid
    VS_START,        // the first accepted view: open the encoder, start the stream
    VS_REFRESH,      // a repeat: the deadline moves; the address may have moved with it
} video_sub_verdict_t;

static inline void video_sub_init(video_sub_t *s) { memset(s, 0, sizeof(*s)); }

// `owner_sid` is rt_link's owner ("" when nobody). *force_idr is set only when the view
// asked for a keyframe and VIDEO_IDR_MIN_MS has passed since the last one forced — never
// on VS_START, since a fresh stream begins with an IDR anyway.
static inline video_sub_verdict_t video_sub_view(video_sub_t *s, const char *owner_sid,
                                                 const control_frame_t *f, uint32_t now_ms,
                                                 bool *force_idr) {
    *force_idr = false;
    if (f->type != CT_VIEW) return VS_IGNORE;
    if (!f->has_proto || f->proto != RT_PROTO) return VS_IGNORE;
    if (owner_sid == NULL || owner_sid[0] == '\0' || strcmp(owner_sid, f->sid) != 0) return VS_IGNORE;
    s->last_view_ms = now_ms;
    if (!s->subscribed) {
        s->subscribed = true;
        s->have_idr_ms = false;
        return VS_START;
    }
    if (f->has_key && f->key) {
        if (!s->have_idr_ms || watchdog_stale(s->last_idr_ms, now_ms, VIDEO_IDR_MIN_MS)) {
            *force_idr = true;
            s->have_idr_ms = true;
            s->last_idr_ms = now_ms;
        }
    }
    return VS_REFRESH;
}

// True when a live subscription should end: the phone stopped asking, or the rt session
// it was tied to is gone.
static inline bool video_sub_expired(const video_sub_t *s, bool owner_alive, uint32_t now_ms) {
    if (!s->subscribed) return false;
    if (!owner_alive) return true;
    return watchdog_stale(s->last_view_ms, now_ms, VIDEO_SUBSCRIBE_TIMEOUT_MS);
}

static inline void video_sub_end(video_sub_t *s) { video_sub_init(s); }

#endif // VIDEO_SUB_H
