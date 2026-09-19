// Host test for CalibGate — whether the drive screen must hold the mandatory calibration
// wizard over itself, from the car's `motors.calibrated` flag and the clock (openspec
// `app/calibration`, «Два входа — по желанию и по требованию»).
//
// The rule replaced an `onChange` on the flag, which only ever saw a *change*: a car that was
// uncalibrated when the session opened never produced one, and the wizard never opened
// (AJM-63). What is checked here is that the verdict is a function of the frame, the first one
// included, and that the only memory is the two-second window after the car was last known
// calibrated.
import Foundation

var failures = 0
func check(_ ok: Bool, _ what: String) {
    if !ok { print("FAIL: \(what)"); failures += 1 }
}

// The bug: the very first frame of a session says false, and that alone is the verdict.
do {
    var g = CalibGate()
    check(g.frame(calibrated: false, now: 100) == true, "first frame false: the wizard is required")
    check(g.frame(calibrated: false, now: 100.2) == true, "and stays required frame after frame")
}

// A calibrated car needs nothing.
do {
    var g = CalibGate()
    check(g.frame(calibrated: true, now: 100) == false, "first frame true: not required")
}

// The flag flips on an open screen: true, then false well after — required.
do {
    var g = CalibGate()
    check(g.frame(calibrated: true, now: 100) == false, "true: not required")
    check(g.frame(calibrated: false, now: 103) == true, "false three seconds after the last true: required")
    check(g.frame(calibrated: true, now: 103.2) == false, "true again: closed")
}

// The two-second window after the last `true`: the frame the car computed before the write
// still says false, and judged alone would reopen the sheet mid-dismiss.
do {
    var g = CalibGate()
    _ = g.frame(calibrated: true, now: 100)
    check(g.frame(calibrated: false, now: 101.9) == false, "false within two seconds of a true: stale, not required")
    check(g.frame(calibrated: false, now: 102.0) == false, "at exactly two seconds: still within the window")
    check(g.frame(calibrated: false, now: 102.001) == true, "just past two seconds: required")
}

// The same window after a save the car accepted — the wizard closed on its own, and the
// telemetry has not caught up yet.
do {
    var g = CalibGate()
    check(g.frame(calibrated: false, now: 100) == true, "uncalibrated: required")
    g.saved(now: 110)
    check(g.frame(calibrated: false, now: 110.4) == false, "stale false right after the accepted save: not required")
    check(g.frame(calibrated: false, now: 112.0) == false, "two seconds after the save: still not required")
    check(g.frame(calibrated: false, now: 112.5) == true, "the car still says false past the window: required again")
    check(g.frame(calibrated: true, now: 112.6) == false, "the car caught up: not required")
}

// The gallery renders the drive screen statically: no session, no wizard — whatever the frame.
do {
    var g = CalibGate(preview: true)
    check(g.frame(calibrated: false, now: 100) == false, "preview: never required")
    check(g.frame(calibrated: false, now: 200) == false, "preview: never required, however long")
}

// No frame is nothing to judge — not an uncalibrated car.
do {
    var g = CalibGate()
    check(g.frame(calibrated: nil, now: 100) == false, "no frame: not required")
    check(g.frame(calibrated: false, now: 100.2) == true, "the first real frame is still judged on its own")
}

// A new session is a new gate: what the previous one saw is not its memory. Its own first
// frame is the verdict, even when the previous session was calibrated a moment ago.
do {
    var old = CalibGate()
    _ = old.frame(calibrated: true, now: 100)
    var fresh = CalibGate()
    check(fresh.frame(calibrated: false, now: 100.5) == true, "a fresh gate judges by its own first frame")
    check(fresh != old, "the fresh gate carries none of the old one's memory")
}

if failures == 0 { print("calibgate: ok") } else { print("calibgate: \(failures) failure(s)"); exit(1) }
