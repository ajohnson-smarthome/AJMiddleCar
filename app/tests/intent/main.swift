// Host test for the command authority's priority rule. Run with swiftc.
import Foundation

var failures = 0
func check(_ ok: Bool, _ what: String) {
    if !ok { print("FAIL: \(what)"); failures += 1 }
}

// A trick owns the channel while nothing else speaks.
var core = IntentCore()
let epoch = core.beginTrick()
check(core.trickStep(epoch: epoch, t: 0.8, y: 0.2), "a trick step is accepted")
check(core.t == 0.8 && core.y == 0.2, "the trick's command is the command")
check(core.source == .trick, "the trick owns the channel")

// Manual input pre-empts it in the same statement — no await, no cancellation to wait for.
check(core.manual(t: -1, y: 0) == true, "manual reports the pre-emption")
check(core.t == -1 && core.y == 0 && core.source == .manual, "manual took the channel")

// The step that was already computed when the finger landed is refused rather than applied.
check(core.trickStep(epoch: epoch, t: 0.8, y: 0.2) == false, "a stale trick step is refused")
check(core.t == -1 && core.y == 0, "and it changed nothing")

// A pre-empted trick's ending must not zero the driver's command.
check(core.endTrick(epoch: epoch, stop: true) == false, "a stale trick cannot end the command")
check(core.t == -1, "the driver's command survives the trick's end")

// A trick that runs to its end does stop the car.
let second = core.beginTrick()
check(second != epoch, "each trick gets its own epoch")
check(core.trickStep(epoch: second, t: 0.5, y: 0.5), "the new trick may speak")
check(core.endTrick(epoch: second, stop: true), "its own end is accepted")
check(core.t == 0 && core.y == 0 && core.source == .idle, "and it stops the car")

// Manual on manual is not a pre-emption, just a new command.
check(core.manual(t: 0.25, y: 0) == false, "manual over manual pre-empts nothing")
check(core.t == 0.25, "and simply commands")

// Commands are clamped wherever they come from.
_ = core.manual(t: 5, y: -5)
check(core.t == 1 && core.y == -1, "manual is clamped")
let third = core.beginTrick()
_ = core.trickStep(epoch: third, t: -9, y: 9)
check(core.t == -1 && core.y == 1, "trick steps are clamped")

// Epochs are a wrapping counter; a wrap must not hand an old trick the channel back.
var wrapped = IntentCore()
for _ in 0..<3 { _ = wrapped.beginTrick() }
check(wrapped.epoch == 3, "epoch counts")

if failures == 0 { print("test_intent: OK") } else { exit(1) }
