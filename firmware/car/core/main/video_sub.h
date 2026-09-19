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
    char     sid[CONTROL_SID_MAX];   // the rt session it opened under; "" while not subscribed
    uint32_t last_view_ms;
    bool     have_idr_ms;
    uint32_t last_idr_ms;
} video_sub_t;

typedef enum {
    VS_IGNORE = 0,   // not a view, a foreign proto, not the owner's sid — or the owner's sid
                     // while a subscription under another still stands (below)
    VS_START,        // the first accepted view: open the encoder, start the stream
    VS_REFRESH,      // a repeat: the deadline moves; the address may have moved with it
} video_sub_verdict_t;

// Why a live subscription is over — VS_ALIVE is zero so `if (video_sub_expired(...))` reads
// as before; the rest is for the log line, so the bench can tell an eviction from a timeout.
typedef enum {
    VS_ALIVE = 0,
    VS_END_TIMEOUT,  // the phone stopped asking
    VS_END_OWNER,    // the rt session is no longer owned by the sid this opened under:
                     // by nobody (bye, idling out) or by somebody else (evicted by another hello)
    VS_END_SWITCH,   // video.enabled went off
} video_sub_end_t;

static inline void video_sub_init(video_sub_t *s) { memset(s, 0, sizeof(*s)); }

// `owner_sid` is rt_link's owner ("" when nobody). `enabled` is the video switch
// (`video.enabled` in /config): off, every view is ignored, whoever sends it — the switch is
// one more reason there is no subscription, not a state of its own. *force_idr is set only
// when the view asked for a keyframe and VIDEO_IDR_MIN_MS has passed since the last one
// forced — never on VS_START, since a fresh stream begins with an IDR anyway.
//
// A subscription belongs to the sid it opened under (AJM-40). Once another hello has taken
// the rt session, the new owner's view is not a refresh of the old owner's subscription —
// accepting it would carry the running stream, mid-GOP, to the new address. It is ignored;
// the same tick's video_sub_expired ends the subscription (VS_END_OWNER), and the new
// owner's next view opens a stream of its own, from a clean slate, at most subscribe_ms later.
static inline video_sub_verdict_t video_sub_view(video_sub_t *s, const char *owner_sid,
                                                 const control_frame_t *f, uint32_t now_ms,
                                                 bool enabled, bool *force_idr) {
    *force_idr = false;
    if (!enabled) return VS_IGNORE;
    if (f->type != CT_VIEW) return VS_IGNORE;
    if (!f->has_proto || f->proto != RT_PROTO) return VS_IGNORE;
    if (owner_sid == NULL || owner_sid[0] == '\0' || strcmp(owner_sid, f->sid) != 0) return VS_IGNORE;
    if (!s->subscribed) {
        s->subscribed = true;
        memcpy(s->sid, f->sid, sizeof(s->sid));
        s->last_view_ms = now_ms;
        s->have_idr_ms = false;
        return VS_START;
    }
    if (strcmp(s->sid, f->sid) != 0) return VS_IGNORE;
    s->last_view_ms = now_ms;
    if (f->has_key && f->key) {
        if (!s->have_idr_ms || watchdog_stale(s->last_idr_ms, now_ms, VIDEO_IDR_MIN_MS)) {
            *force_idr = true;
            s->have_idr_ms = true;
            s->last_idr_ms = now_ms;
        }
    }
    return VS_REFRESH;
}

// Why a live subscription should end, VS_ALIVE when it should not: the phone stopped
// asking; the rt session's owner (`owner_sid`, "" when nobody) is no longer the sid this
// opened under — gone, or replaced by another hello; or the switch went off. The last two
// are judged on every call, which is why a stream ends on the control task's next tick
// rather than at the timeout. The switch is judged first: off is off, whoever owns.
static inline video_sub_end_t video_sub_expired(const video_sub_t *s, const char *owner_sid,
                                                bool enabled, uint32_t now_ms) {
    if (!s->subscribed) return VS_ALIVE;
    if (!enabled) return VS_END_SWITCH;
    if (owner_sid == NULL || strcmp(owner_sid, s->sid) != 0) return VS_END_OWNER;
    return watchdog_stale(s->last_view_ms, now_ms, VIDEO_SUBSCRIBE_TIMEOUT_MS) ? VS_END_TIMEOUT : VS_ALIVE;
}

static inline void video_sub_end(video_sub_t *s) { video_sub_init(s); }

#endif // VIDEO_SUB_H
