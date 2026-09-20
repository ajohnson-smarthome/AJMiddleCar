// Host test for PicturePresence — whether the drive screen has a picture, and the moment it
// appeared (openspec `app/drive-hud`, «„Нет картинки“ — поверх последнего кадра, езда не
// прерывается»).
//
// The «Нет картинки» placard used to hang on a flag published by the one-second stats tick,
// so the first frame reached the window up to a second before the placard left (AJM-175).
// The rule: a frame makes the picture present at once and `frame(at:)` says so only on that
// transition — the receiver hops to the main actor once per appearance, not per frame; the
// tick keeps the reverse transition, `staleAfter` without a frame, and `reset()` is a fresh
// socket that owes its own first frame.
import Foundation

var failures = 0
func check(_ ok: Bool, _ what: String) {
    if !ok { print("FAIL: \(what)"); failures += 1 }
}

let t0 = Date(timeIntervalSinceReferenceDate: 1000)
let stale = PicturePresence.staleAfter

check(stale == 2, "two seconds without a frame is «Нет картинки», as before")

// Fresh: no picture.
do {
    let p = PicturePresence()
    check(!p.present(at: t0), "fresh: no picture")
}

// The first frame: present, and a transition.
do {
    var p = PicturePresence()
    check(p.frame(at: t0), "first frame: the picture appeared")
    check(p.present(at: t0), "…and is present at that moment")
    check(p.present(at: t0 + 1), "…and a second later")
}

// A second frame while the picture is fresh: present, not a transition.
do {
    var p = PicturePresence()
    _ = p.frame(at: t0)
    check(!p.frame(at: t0 + 0.5), "second frame half a second later: not a transition")
    check(p.present(at: t0 + 0.5), "…but present")
}

// Silence for `staleAfter`: gone.
do {
    var p = PicturePresence()
    _ = p.frame(at: t0)
    _ = p.frame(at: t0 + 0.5)
    check(p.present(at: t0 + 0.5 + stale - 0.01), "just under the threshold: still present")
    check(!p.present(at: t0 + 0.5 + stale), "the threshold itself: gone")
    check(!p.present(at: t0 + 60), "long after: gone")
}

// A frame after the silence: a transition again.
do {
    var p = PicturePresence()
    _ = p.frame(at: t0)
    check(p.frame(at: t0 + stale + 1), "frame after the silence: the picture appeared again")
    check(p.present(at: t0 + stale + 1), "…and is present")
}

// Reset: no picture, and the next frame is an appearance — a reopened socket owes its own
// first frame, whatever the previous one delivered.
do {
    var p = PicturePresence()
    _ = p.frame(at: t0)
    p.reset()
    check(!p.present(at: t0), "reset: no picture, even at the moment of the last frame")
    check(p.frame(at: t0 + 0.1), "the first frame after reset is a transition")
}

if failures > 0 { print("picturepresence: \(failures) failure(s)"); exit(1) }
print("picturepresence: ok")
