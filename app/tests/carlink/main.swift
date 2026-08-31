// Host test for the link composition truth table. Run with swiftc.
import Foundation
import Network

var failures = 0
func check(_ ok: Bool, _ what: String) {
    if !ok { print("FAIL: \(what)"); failures += 1 }
}

var fresh = Telemetry()
fresh.uptimeS = 10

let adopted = SessionState.adopted(device: CarContract.device, fw: "v1.0+517")
func compose(_ p: PathState, _ s: SessionState, _ t: Telemetry?, _ age: TimeInterval?) -> Link {
    LinkRule.compose(path: p, session: s, telemetry: t, age: age)
}

// All three, or it is not live.
check(compose(.dongleUp, adopted, fresh, 0.1) == .live(fresh), "path + session + fresh frame = live")
check(compose(.dongleUp, adopted, fresh, 5) == .searching, "stale telemetry is not live")
check(compose(.dongleUp, adopted, nil, nil) == .searching, "adopted but silent is not live")
check(compose(.dongleUp, .none, fresh, 0.1) == .searching, "telemetry without a session is not live")

// The path outranks everything: no amount of remembered telemetry survives losing the interface.
check(compose(.noDongle(.notAvailable), adopted, fresh, 0.1) == .noDongle(.notAvailable), "no dongle wins")
check(compose(.localNetworkDenied, adopted, fresh, 0.1) == .localNetworkDenied, "denial wins")
check(compose(.localNetworkDenied, .foreign(device: "esp32-car"), nil, nil) == .localNetworkDenied,
      "denial outranks a foreign car")

// Identity outranks liveness: a car that answers with someone else's name is never driven.
check(compose(.dongleUp, .foreign(device: "esp32-car"), fresh, 0.1) == .wrongCar(device: "esp32-car"),
      "wrong car, however fresh")

// Nor is one that answers in a protocol version this build does not speak — and it gets a screen
// that says so rather than the radar it would otherwise sweep forever.
check(compose(.dongleUp, .protoMismatch(theirs: CarContract.proto + 1), fresh, 0.1)
        == .wrongProto(theirs: CarContract.proto + 1), "a protocol mismatch is its own state")
check(compose(.dongleUp, .protoMismatch(theirs: 2), nil, nil) == .wrongProto(theirs: 2),
      "reported before any telemetry, which is when it happens")
check(compose(.localNetworkDenied, .protoMismatch(theirs: 2), nil, nil) == .localNetworkDenied,
      "the path still outranks it")
check(!compose(.dongleUp, .protoMismatch(theirs: 2), fresh, 0.1).isLive, "and it is never live")

// What survives a session ending. Two callers ask this — the session closing under the transport,
// and `stop(graceful:)` when the scene leaves `.active` — and when it lived twice as an `if case`,
// `.protoMismatch` was added to one copy and not the other: a Control Center pull-down flipped the
// wrong-protocol screen back to the radar until the next hello reply landed.
check(SessionState.foreign(device: "esp32-car").survivingSessionEnd == .foreign(device: "esp32-car"),
      "a foreign identity survives the session that found it")
check(SessionState.protoMismatch(theirs: 2).survivingSessionEnd == .protoMismatch(theirs: 2),
      "and so does a protocol mismatch — a backgrounded app must not forget it")
check(adopted.survivingSessionEnd == .none, "an adopted session does not")
check(SessionState.none.survivingSessionEnd == .none, "nor does nothing at all")
// Which is the whole point: both survivors keep their own screen across the restart.
check(compose(.dongleUp, SessionState.protoMismatch(theirs: 2).survivingSessionEnd, nil, nil)
        == .wrongProto(theirs: 2), "the wrong-protocol screen holds across a session end")
check(compose(.dongleUp, SessionState.foreign(device: "esp32-car").survivingSessionEnd, nil, nil)
        == .wrongCar(device: "esp32-car"), "the wrong-car screen holds across a session end")

// The staleness threshold is the contract's telemetry rate, not a number picked here.
check(LinkRule.staleAfter == 5 / Double(CarContract.telemetryHz), "staleness from the contract")
check(compose(.dongleUp, adopted, fresh, LinkRule.staleAfter - 0.01).isLive, "just inside the window")
check(!compose(.dongleUp, adopted, fresh, LinkRule.staleAfter).isLive, "the boundary is not live")

if failures == 0 { print("test_carlink: OK") } else { exit(1) }
