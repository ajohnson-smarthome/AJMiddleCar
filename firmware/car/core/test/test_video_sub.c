#define RT_LINK_HOST_TEST
#include "../main/video_sub.h"
#include "contract.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static control_frame_t view(const char *sid, bool with_key, bool key) {
    char msg[96];
    if (with_key) snprintf(msg, sizeof(msg), "{\"proto\":2,\"type\":\"view\",\"session\":\"%s\",\"key\":%s}", sid, key ? "true" : "false");
    else          snprintf(msg, sizeof(msg), "{\"proto\":2,\"type\":\"view\",\"session\":\"%s\"}", sid);
    control_frame_t f;
    assert(control_parse_frame(msg, strlen(msg), RT_MAX_COMMAND, &f) == 0);
    return f;
}

int main(void) {
    video_sub_t s;
    video_sub_init(&s);
    bool idr = true;
    control_frame_t f = view("7f3a91c2", false, false);

    /* Nobody owns the rt session: every view is ignored. */
    assert(video_sub_view(&s, "", &f, 1000, true, &idr) == VS_IGNORE && !idr && !s.subscribed);
    /* Another session's sid: ignored. */
    assert(video_sub_view(&s, "deadbeef", &f, 1000, true, &idr) == VS_IGNORE && !s.subscribed);
    /* The owner's sid: starts the stream; no IDR to force — a new stream opens with one.
       The subscription remembers the sid it opened under. */
    idr = true;
    assert(video_sub_view(&s, "7f3a91c2", &f, 1000, true, &idr) == VS_START && !idr && s.subscribed);
    assert(strcmp(s.sid, "7f3a91c2") == 0);
    /* Repeats refresh. */
    assert(video_sub_view(&s, "7f3a91c2", &f, 1900, true, &idr) == VS_REFRESH && !idr);
    /* Not expired within the timeout (watchdog_stale is strict: `>`); expired past it. */
    assert(video_sub_expired(&s, "7f3a91c2", true, 1900 + VIDEO_SUBSCRIBE_TIMEOUT_MS) == VS_ALIVE);
    assert(video_sub_expired(&s, "7f3a91c2", true, 1900 + VIDEO_SUBSCRIBE_TIMEOUT_MS + 1) == VS_END_TIMEOUT);
    /* The rt session ending (nobody owns it) expires it at once. */
    assert(video_sub_expired(&s, "", true, 1901) == VS_END_OWNER);
    /* A foreign proto on a view is ignored even from the owner. */
    control_frame_t foreign = f; foreign.proto = 7;
    assert(video_sub_view(&s, "7f3a91c2", &foreign, 2000, true, &idr) == VS_IGNORE);
    control_frame_t noproto = f; noproto.has_proto = false;
    assert(video_sub_view(&s, "7f3a91c2", &noproto, 2000, true, &idr) == VS_IGNORE);
    /* key:true forces an IDR, rate-limited. */
    control_frame_t k = view("7f3a91c2", true, true);
    assert(video_sub_view(&s, "7f3a91c2", &k, 2000, true, &idr) == VS_REFRESH && idr);
    assert(video_sub_view(&s, "7f3a91c2", &k, 2000 + VIDEO_IDR_MIN_MS, true, &idr) == VS_REFRESH && !idr);
    assert(video_sub_view(&s, "7f3a91c2", &k, 2000 + VIDEO_IDR_MIN_MS + 1, true, &idr) == VS_REFRESH && idr);
    control_frame_t kf = view("7f3a91c2", true, false);
    assert(video_sub_view(&s, "7f3a91c2", &kf, 3000, true, &idr) == VS_REFRESH && !idr);
    /* Ending clears everything; the next view starts again. */
    video_sub_end(&s);
    assert(!s.subscribed && s.sid[0] == '\0');
    assert(video_sub_expired(&s, "7f3a91c2", true, 99999) == VS_ALIVE);
    assert(video_sub_expired(&s, "", true, 99999) == VS_ALIVE);
    assert(video_sub_view(&s, "7f3a91c2", &f, 5000, true, &idr) == VS_START);
    /* Only a view counts: a hello carrying the owner's sid is not a subscription. */
    control_frame_t hello;
    const char *hm = "{\"proto\":2,\"type\":\"hello\",\"session\":\"7f3a91c2\"}";
    assert(control_parse_frame(hm, strlen(hm), RT_MAX_COMMAND, &hello) == 0);
    assert(video_sub_view(&s, "7f3a91c2", &hello, 5100, true, &idr) == VS_IGNORE);
    /* The clock wraps: expiry arithmetic is unsigned. */
    video_sub_init(&s);
    assert(video_sub_view(&s, "7f3a91c2", &f, 0xFFFFFF00u, true, &idr) == VS_START);
    assert(video_sub_expired(&s, "7f3a91c2", true, 0xFFFFFF00u + 100) == VS_ALIVE);
    assert(video_sub_expired(&s, "7f3a91c2", true, 0xFFFFFF00u + VIDEO_SUBSCRIBE_TIMEOUT_MS + 1) == VS_END_TIMEOUT);

    /* The switch (spec 2026-09-15): off, the owner's view is ignored — nothing opens. */
    video_sub_init(&s);
    assert(video_sub_view(&s, "7f3a91c2", &f, 7000, false, &idr) == VS_IGNORE && !s.subscribed && !idr);
    assert(video_sub_view(&s, "7f3a91c2", &k, 7000, false, &idr) == VS_IGNORE && !idr);
    /* On: opens as before. Then the flag drops mid-stream — the subscription is over on
       that very call, not at the timeout. */
    assert(video_sub_view(&s, "7f3a91c2", &f, 7100, true, &idr) == VS_START);
    assert(video_sub_expired(&s, "7f3a91c2", true, 7200) == VS_ALIVE);
    assert(video_sub_expired(&s, "7f3a91c2", false, 7200) == VS_END_SWITCH);
    /* Back on: nothing opens by itself — only the next view does. */
    video_sub_end(&s);
    assert(!s.subscribed);
    assert(video_sub_expired(&s, "7f3a91c2", true, 7300) == VS_ALIVE);
    assert(video_sub_view(&s, "7f3a91c2", &f, 7400, true, &idr) == VS_START);

    /* Eviction (AJM-40): the subscription opened under one sid; another hello took the rt
       session. The tick that sees the new owner ends it — well inside the timeout — and no
       chunk goes to the old address after that tick. */
    video_sub_init(&s);
    assert(video_sub_view(&s, "7f3a91c2", &f, 8000, true, &idr) == VS_START);
    assert(video_sub_expired(&s, "7f3a91c2", true, 8100) == VS_ALIVE);
    assert(video_sub_expired(&s, "b2c3d4e5", true, 8100) == VS_END_OWNER);
    /* The new owner's first view on the old owner's live subscription is not a refresh of
       it — it must not drag the old stream to the new address; the same tick ends the
       subscription, and the next view opens a stream of its own. */
    control_frame_t f2 = view("b2c3d4e5", true, true);
    idr = true;
    assert(video_sub_view(&s, "b2c3d4e5", &f2, 8100, true, &idr) == VS_IGNORE && !idr);
    assert(s.subscribed && strcmp(s.sid, "7f3a91c2") == 0 && s.last_view_ms == 8000);
    assert(video_sub_expired(&s, "b2c3d4e5", true, 8100) == VS_END_OWNER);
    /* The evicted owner's own view is no longer the owner's: ignored, as before. */
    assert(video_sub_view(&s, "b2c3d4e5", &f, 8100, true, &idr) == VS_IGNORE);
    video_sub_end(&s);
    /* With a clean slate, the new owner's view starts a stream under its sid. */
    assert(video_sub_view(&s, "b2c3d4e5", &f2, 9100, true, &idr) == VS_START && !idr);
    assert(strcmp(s.sid, "b2c3d4e5") == 0);
    assert(video_sub_expired(&s, "b2c3d4e5", true, 9200) == VS_ALIVE);
    assert(video_sub_expired(&s, "7f3a91c2", true, 9200) == VS_END_OWNER);
    /* The switch outranks the owner check in the verdict: off is off, whoever owns. */
    assert(video_sub_expired(&s, "7f3a91c2", false, 9200) == VS_END_SWITCH);

    printf("test_video_sub: OK\n");
    return 0;
}
