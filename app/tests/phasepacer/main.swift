// Host test for PhasePacer — the rule that decides how long a startup screen is on screen for.
//
// The rule exists because six meaningful steps resolving in milliseconds are six flashes, not a
// sequence. What is checked here is the trade-off it makes: nothing is skipped, a step slower
// than the floor is never delayed, and the worst case is bounded and computable.
import Foundation

var failures = 0
func check(_ ok: Bool, _ what: String) {
    if !ok { print("FAIL: \(what)"); failures += 1 }
}
func near(_ a: [TimeInterval], _ b: [TimeInterval]) -> Bool {
    a.count == b.count && zip(a, b).allSatisfy { abs($0 - $1) < 1e-9 }
}

let M = 0.4

// A launch where everything is instant is the case the rule exists for: without it, six screens
// share one frame. With it, each gets its own slot and the whole ladder costs 5 × minVisible
// after the first — the number worth arguing about, stated rather than felt.
check(near(PhasePacer.schedule(arrivals: [0, 0, 0, 0, 0, 0], minVisible: M),
           [0, 0.4, 0.8, 1.2, 1.6, 2.0]),
      "six instant steps are paced one per slot")

// A step that genuinely takes time is never delayed: the pacing is a floor, not a metronome.
check(near(PhasePacer.schedule(arrivals: [0, 3, 6], minVisible: M), [0, 3, 6]),
      "slow steps pass through untouched")

// Mixed: the burst is paced, and the moment real time exceeds the floor the schedule snaps back
// to the arrivals — the delay does not accumulate across a whole launch.
check(near(PhasePacer.schedule(arrivals: [0, 0.1, 0.2, 5.0], minVisible: M),
           [0, 0.4, 0.8, 5.0]),
      "the pacing debt is repaid, not carried")

// The first screen is never held back. A launch must not begin with a blank pause.
check(PhasePacer.schedule(arrivals: [1.7], minVisible: M) == [1.7], "the first screen shows at once")

// Nothing is dropped and nothing runs backwards.
let arrivals: [TimeInterval] = [0, 0, 0.05, 0.05, 0.9, 0.9, 0.95, 4]
let sched = PhasePacer.schedule(arrivals: arrivals, minVisible: M)
check(sched.count == arrivals.count, "every step is scheduled, none skipped")
check(zip(sched, sched.dropFirst()).allSatisfy { $1 >= $0 + M - 1e-9 },
      "consecutive screens are always at least minVisible apart")
check(zip(arrivals, sched).allSatisfy { $1 >= $0 - 1e-9 },
      "no screen is shown before its state was true")

check(PhasePacer.schedule(arrivals: [], minVisible: M).isEmpty, "an empty launch schedules nothing")

// The incremental form the app actually calls, and the one that has to agree with the schedule.
check(abs(PhasePacer.wait(shownAt: 10, now: 10, minVisible: M) - M) < 1e-9,
      "a screen just shown blocks for the whole floor")
check(PhasePacer.wait(shownAt: 10, now: 10.4, minVisible: M) == 0,
      "a screen that has served its floor blocks nothing")
check(PhasePacer.wait(shownAt: 10, now: 99, minVisible: M) == 0,
      "a long-standing screen never blocks, and never goes negative")

if failures == 0 { print("phasepacer: all checks passed") }
exit(failures == 0 ? 0 : 1)
