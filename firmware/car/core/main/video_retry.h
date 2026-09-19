#ifndef VIDEO_RETRY_H
#define VIDEO_RETRY_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

// The series of failed stream starts, as arithmetic: how many starts in a row delivered
// no frame, whether that already says `off`, and when the next start may be tried. Pure,
// like video_sub_t, so the rule is pinned by test_video_retry.c rather than by the bench.
//
// camera_present() is about boot: the sensor answered once, and `off` was reserved for the
// car that booted without one. A sensor that goes quiet while someone is watching used to
// look different on the wire and worse for the driver — every start failed, every failure
// read `idle` ("nobody watching") while the drive screen was open, and the pipeline was
// reopened once a second with a burst of errors each time (AJM-94). This series is the
// judgement camera.c cannot make: the encode task counts starts that ended without a
// frame, and after VIDEO_RETRY_OFF_AFTER of them the state word is `off` — "the camera is
// not answering" — while the retries continue, rarer, for the sensor that comes back.

// Starts in a row without a frame before the state word says `off`. Each is at most one
// open plus one bounded wait (camera.h: 500 ms), so the word changes within a few seconds
// of the sensor going quiet — and never on one dropped frame or one slow STREAMON.
#define VIDEO_RETRY_OFF_AFTER   3
// Between attempts while the series is short: the pace the stream always had (R9b) —
// a sensor that stopped delivering is not reopened at the 2 Hz its bounded wait would give.
#define VIDEO_RETRY_FAST_MS     1000
// Between attempts once `off`: three starts without a frame is a sensor that is most
// likely gone, and the retries from here are for the one that comes back — rarer than
// once a second, so the log is not a torrent while the drive screen stays open.
#define VIDEO_RETRY_SLOW_MS     5000

typedef struct {
    unsigned failed;      // starts in a row that delivered no frame; saturates at OFF_AFTER
    bool     paused;      // a failure has set the pause below
    uint32_t failed_ms;   // when the last one failed; the pause counts from here
} video_retry_t;

static inline void video_retry_init(video_retry_t *r) { memset(r, 0, sizeof(*r)); }

// Whether the series says `off`.
static inline bool video_retry_off(const video_retry_t *r) { return r->failed >= VIDEO_RETRY_OFF_AFTER; }

// Whether a start may be tried at now_ms: always until one fails, then not before the
// pause the last failure set — fast while the series is short, slow once it says `off`.
// Unsigned subtraction, so the millisecond clock may wrap under it.
static inline bool video_retry_due(const video_retry_t *r, uint32_t now_ms) {
    if (!r->paused) return true;
    uint32_t pause = video_retry_off(r) ? VIDEO_RETRY_SLOW_MS : VIDEO_RETRY_FAST_MS;
    return (uint32_t)(now_ms - r->failed_ms) >= pause;
}

// A start that delivered no frame — the open failed, or the wait for a frame ran out —
// or a running stream whose sensor went quiet. The series grows; the pause restarts.
static inline void video_retry_failed(video_retry_t *r, uint32_t now_ms) {
    if (r->failed < VIDEO_RETRY_OFF_AFTER) r->failed++;
    r->paused = true;
    r->failed_ms = now_ms;
}

// A frame arrived: the sensor is answering, whatever it did before. The series is over,
// and the next start — after this stream ends — may be tried at once.
static inline void video_retry_delivered(video_retry_t *r) { video_retry_init(r); }

#endif // VIDEO_RETRY_H
