// Host test for VideoReopenHold — when the video socket may (re)open after the car accepted a
// new bitrate (openspec `app/drive-hud`, «Подписка `view` — пока есть сессия и экран смотрит»).
//
// The car reads `bitrate_kbps` once, at stream start, and a `view` inside
// `video.subscribe_timeout_ms` of the last one refreshes the running stream instead of
// starting a new one. A slider moved under the settings sheet and the sheet closed within
// three seconds therefore changed nothing the driver could see (AJM-122). The hold makes the
// next open wait until the car's subscription has certainly lapsed.
import Foundation

var failures = 0
func check(_ ok: Bool, _ what: String) {
    if !ok { print("FAIL: \(what)"); failures += 1 }
}

let timeout = Double(CarContract.videoSubscribeTimeoutMs) / 1000

// Which accepted writes need the stream to lapse: a bitrate change while the switch stays on.
let on25 = Video(bitrate_kbps: 2500, enabled: true)
let on15 = Video(bitrate_kbps: 1500, enabled: true)
let off25 = Video(bitrate_kbps: 2500, enabled: false)
let off15 = Video(bitrate_kbps: 1500, enabled: false)
check(VideoReopenHold.restartsStream(from: on25, to: on15), "bitrate changed, switch on: the stream must lapse")
check(!VideoReopenHold.restartsStream(from: on25, to: on25), "same domain: nothing to wait for")
check(!VideoReopenHold.restartsStream(from: on25, to: off25), "switched off: the car ends the stream itself")
check(!VideoReopenHold.restartsStream(from: on25, to: off15), "switched off with a new bitrate: same")
check(!VideoReopenHold.restartsStream(from: off25, to: off15), "bitrate changed while off: no stream to restart")
check(!VideoReopenHold.restartsStream(from: off15, to: on15), "switched on: the next view starts a stream that reads the new bitrate")

// No hold: open at once.
do {
    let h = VideoReopenHold()
    check(h.delay(now: 100) == nil, "no hold: open now")
}

// Armed from the last view sent: the socket may open again only once the car's subscription
// timeout has passed since then — with the margin, never before the timeout itself.
do {
    var h = VideoReopenHold()
    h.arm(lastViewAt: 100)
    guard let atOnce = h.delay(now: 100.5) else { print("FAIL: armed: must wait"); exit(1) }
    check(atOnce >= timeout - 0.5, "the wait is at least the rest of the timeout")
    check(atOnce <= timeout - 0.5 + VideoReopenHold.margin + 1e-9, "…plus the margin, no more")
    check(h.delay(now: 100 + timeout) != nil, "at exactly the timeout: still held (the car's clock ticks late)")
    check(h.delay(now: 100 + timeout + VideoReopenHold.margin) == nil, "timeout plus margin: free")
    check(h.delay(now: 200) == nil, "long after: free")
}

// Armed with the socket already silent for a while: less to wait, never negative.
do {
    var h = VideoReopenHold()
    h.arm(lastViewAt: 100)
    check(h.delay(now: 100 + timeout + VideoReopenHold.margin - 0.1).map { $0 > 0 && $0 <= 0.1 + 1e-9 } ?? false,
          "almost lapsed: a short wait")
}

// Re-arming later moves the hold later, never earlier.
do {
    var h = VideoReopenHold()
    h.arm(lastViewAt: 100)
    h.arm(lastViewAt: 101)
    check(h.delay(now: 100 + timeout + VideoReopenHold.margin) != nil, "the later view is the one that counts")
    check(h.delay(now: 101 + timeout + VideoReopenHold.margin) == nil, "…and it lapses on its own schedule")
}

// The margin is small change next to the timeout: it covers the car's subscription clock
// (100 ms ticks) and the relay's latency, not a second black screen.
check(VideoReopenHold.margin > 0 && VideoReopenHold.margin < 1, "a margin, not a second timeout")

if failures > 0 { print("videohold: \(failures) failure(s)"); exit(1) }
print("videohold: ok")
