#include "../main/video_retry.h"
#include <assert.h>
#include <stdio.h>

/* The series of failed starts, as arithmetic (AJM-94): a sensor that went quiet while
   someone was watching used to read `idle` — "nobody watching" — for as long as the drive
   screen stayed open, and the pipeline was reopened every second. Now N starts in a row
   that deliver no frame say `off`, and the retries after that are rarer. */
int main(void) {
    _Static_assert(VIDEO_RETRY_OFF_AFTER >= 2, "one bounded wait is a dropped frame, not a dead sensor");
    _Static_assert(VIDEO_RETRY_SLOW_MS > 1000, "once off, retries are rarer than once a second");
    _Static_assert(VIDEO_RETRY_SLOW_MS > VIDEO_RETRY_FAST_MS, "off slows the retries down, not up");

    video_retry_t r;
    video_retry_init(&r);

    /* Fresh: nothing has failed, a start may be tried at once. */
    assert(!video_retry_off(&r));
    assert(video_retry_due(&r, 0));
    assert(video_retry_due(&r, 123456));

    /* One failed start: not off — a single 500 ms wait without a frame is a dropped frame
       or a slow STREAMON, not a dead sensor — and the next attempt waits the fast pause,
       the pace the stream always had. */
    video_retry_failed(&r, 1000);
    assert(!video_retry_off(&r));
    assert(!video_retry_due(&r, 1000));
    assert(!video_retry_due(&r, 1000 + VIDEO_RETRY_FAST_MS - 1));
    assert(video_retry_due(&r, 1000 + VIDEO_RETRY_FAST_MS));

    /* Up to N-1 in a row: still not off, still the fast pause. */
    for (unsigned i = 2; i < VIDEO_RETRY_OFF_AFTER; i++) {
        video_retry_failed(&r, 1000 * i);
        assert(!video_retry_off(&r));
        assert(!video_retry_due(&r, 1000 * i + VIDEO_RETRY_FAST_MS - 1));
        assert(video_retry_due(&r, 1000 * i + VIDEO_RETRY_FAST_MS));
    }

    /* The N-th: off. The word changes, and the retries slow down — the next attempt is not
       before the slow pause, so the log is not a torrent while the screen stays open. */
    uint32_t t = 1000 * VIDEO_RETRY_OFF_AFTER;
    video_retry_failed(&r, t);
    assert(video_retry_off(&r));
    assert(!video_retry_due(&r, t));
    assert(!video_retry_due(&r, t + VIDEO_RETRY_FAST_MS));
    assert(!video_retry_due(&r, t + VIDEO_RETRY_SLOW_MS - 1));
    assert(video_retry_due(&r, t + VIDEO_RETRY_SLOW_MS));

    /* Failures keep coming at the slow pace: still off, the counter does not wrap back to
       "not off" however long the sensor stays dead. */
    for (unsigned i = 0; i < 100000; i++) {
        t += VIDEO_RETRY_SLOW_MS;
        video_retry_failed(&r, t);
        assert(video_retry_off(&r));
        assert(!video_retry_due(&r, t + VIDEO_RETRY_SLOW_MS - 1));
        assert(video_retry_due(&r, t + VIDEO_RETRY_SLOW_MS));
    }

    /* A frame arrived: the sensor is back. The series is over — not off, no pause — and
       the next series starts from one again. */
    video_retry_delivered(&r);
    assert(!video_retry_off(&r));
    assert(video_retry_due(&r, t));
    video_retry_failed(&r, t + 10);
    assert(!video_retry_off(&r));
    assert(video_retry_due(&r, t + 10 + VIDEO_RETRY_FAST_MS));

    /* A frame in the middle of a short series clears it too: N-1 failures, a frame, then
       N-1 more do not add up to off. */
    video_retry_init(&r);
    for (unsigned i = 1; i < VIDEO_RETRY_OFF_AFTER; i++) video_retry_failed(&r, 100 * i);
    assert(!video_retry_off(&r));
    video_retry_delivered(&r);
    for (unsigned i = 1; i < VIDEO_RETRY_OFF_AFTER; i++) video_retry_failed(&r, 100 * i);
    assert(!video_retry_off(&r));

    /* The clock wraps: the pause is unsigned arithmetic, like every other one in the car. */
    video_retry_init(&r);
    video_retry_failed(&r, 0xFFFFFF00u);
    assert(!video_retry_due(&r, 0xFFFFFF00u + VIDEO_RETRY_FAST_MS - 1));
    assert(video_retry_due(&r, 0xFFFFFF00u + VIDEO_RETRY_FAST_MS));

    printf("test_video_retry: OK\n");
    return 0;
}
