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
    assert(video_sub_view(&s, "", &f, 1000, &idr) == VS_IGNORE && !idr && !s.subscribed);
    /* Another session's sid: ignored. */
    assert(video_sub_view(&s, "deadbeef", &f, 1000, &idr) == VS_IGNORE && !s.subscribed);
    /* The owner's sid: starts the stream; no IDR to force — a new stream opens with one. */
    idr = true;
    assert(video_sub_view(&s, "7f3a91c2", &f, 1000, &idr) == VS_START && !idr && s.subscribed);
    /* Repeats refresh. */
    assert(video_sub_view(&s, "7f3a91c2", &f, 1900, &idr) == VS_REFRESH && !idr);
    /* Not expired within the timeout (watchdog_stale is strict: `>`); expired past it. */
    assert(!video_sub_expired(&s, true, 1900 + VIDEO_SUBSCRIBE_TIMEOUT_MS));
    assert(video_sub_expired(&s, true, 1900 + VIDEO_SUBSCRIBE_TIMEOUT_MS + 1));
    /* The rt session ending expires it at once. */
    assert(video_sub_expired(&s, false, 1901));
    /* A foreign proto on a view is ignored even from the owner. */
    control_frame_t foreign = f; foreign.proto = 7;
    assert(video_sub_view(&s, "7f3a91c2", &foreign, 2000, &idr) == VS_IGNORE);
    control_frame_t noproto = f; noproto.has_proto = false;
    assert(video_sub_view(&s, "7f3a91c2", &noproto, 2000, &idr) == VS_IGNORE);
    /* key:true forces an IDR, rate-limited. */
    control_frame_t k = view("7f3a91c2", true, true);
    assert(video_sub_view(&s, "7f3a91c2", &k, 2000, &idr) == VS_REFRESH && idr);
    assert(video_sub_view(&s, "7f3a91c2", &k, 2000 + VIDEO_IDR_MIN_MS, &idr) == VS_REFRESH && !idr);
    assert(video_sub_view(&s, "7f3a91c2", &k, 2000 + VIDEO_IDR_MIN_MS + 1, &idr) == VS_REFRESH && idr);
    control_frame_t kf = view("7f3a91c2", true, false);
    assert(video_sub_view(&s, "7f3a91c2", &kf, 3000, &idr) == VS_REFRESH && !idr);
    /* Ending clears everything; the next view starts again. */
    video_sub_end(&s);
    assert(!s.subscribed && !video_sub_expired(&s, true, 99999));
    assert(video_sub_view(&s, "7f3a91c2", &f, 5000, &idr) == VS_START);
    /* Only a view counts: a hello carrying the owner's sid is not a subscription. */
    control_frame_t hello;
    const char *hm = "{\"proto\":2,\"type\":\"hello\",\"session\":\"7f3a91c2\"}";
    assert(control_parse_frame(hm, strlen(hm), RT_MAX_COMMAND, &hello) == 0);
    assert(video_sub_view(&s, "7f3a91c2", &hello, 5100, &idr) == VS_IGNORE);
    /* The clock wraps: expiry arithmetic is unsigned. */
    video_sub_init(&s);
    assert(video_sub_view(&s, "7f3a91c2", &f, 0xFFFFFF00u, &idr) == VS_START);
    assert(!video_sub_expired(&s, true, 0xFFFFFF00u + 100));
    assert(video_sub_expired(&s, true, 0xFFFFFF00u + VIDEO_SUBSCRIBE_TIMEOUT_MS + 1));

    printf("test_video_sub: OK\n");
    return 0;
}
