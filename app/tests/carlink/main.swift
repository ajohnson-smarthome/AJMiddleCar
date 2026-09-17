// Host test for the link composition truth table. Run with swiftc.
import Foundation
import Network

var failures = 0
func check(_ ok: Bool, _ what: String) {
    if !ok { print("FAIL: \(what)"); failures += 1 }
}

let fresh = Telemetry(proto: 2, seq: 1,
                      link: LinkInfo(rx_hz: 10, rssi_dbm: -58, timeouts: 0),
                      motors: MotorsInfo(bus: .ok, calibrated: true, owner: .remote),
                      system: SystemInfo(uptime_s: 10, free_heap: 200000),
                      video: VideoInfo(state: .idle, fps: 0, kbps: 0, dropped: 0))

let adopted = SessionState.adopted(device: CarContract.device, fw: "v1.0+517")
func compose(_ p: PathState, _ s: SessionState, _ t: Telemetry?, _ age: TimeInterval?) -> Link {
    LinkRule.compose(path: p, session: s, telemetry: t, age: age)
}

// All three, or it is not live.
check(compose(.dongleUp, adopted, fresh, 0.1) == .live(fresh), "path + session + fresh frame = live")
check(compose(.dongleUp, adopted, fresh, 5) == .searching, "long-stale telemetry is not live")
check(compose(.dongleUp, adopted, nil, nil) == .searching, "adopted but silent is not live")
check(compose(.dongleUp, .none, fresh, 0.1) == .searching, "telemetry without a session is not live")

// The path outranks everything: no amount of remembered telemetry survives losing the interface.
check(compose(.noDongle(.notAvailable), adopted, fresh, 0.1) == .noDongle(.notAvailable), "no dongle wins")
// And the root view reads that state as a transition — "was gone, is back" is what re-enters
// the dongle gate after a replug, so a predicate that answered anything else would either
// re-gate a live session on every telemetry frame or never re-gate at all.
check(compose(.noDongle(.notAvailable), adopted, fresh, 0.1).isNoDongle, "a lost interface says so")
check(!compose(.dongleUp, adopted, fresh, 0.1).isNoDongle, "a live link does not")
check(!compose(.dongleUp, .none, nil, nil).isNoDongle, "and neither does merely searching")
check(!compose(.localNetworkDenied, adopted, fresh, 0.1).isNoDongle,
      "nor a denial — replugging the cable is not what fixes that one")
check(compose(.localNetworkDenied, adopted, fresh, 0.1) == .localNetworkDenied, "denial wins")
check(compose(.localNetworkDenied, .foreign(device: "esp32-car"), nil, nil) == .localNetworkDenied,
      "denial outranks a foreign car")

// Identity outranks liveness: a car that answers with someone else's name is never driven.
check(compose(.dongleUp, .foreign(device: "esp32-car"), fresh, 0.1) == .wrongCar(device: "esp32-car"),
      "wrong car, however fresh")

// What survives a session ending. Two callers ask this — the session closing under the transport,
// and `stop(graceful:)` when the scene leaves `.active` — a foreign identity is not a transient
// failure to retry behind a radar sweep, so it must survive both.
check(SessionState.foreign(device: "esp32-car").survivingSessionEnd == .foreign(device: "esp32-car"),
      "a foreign identity survives the session that found it")
check(adopted.survivingSessionEnd == .none, "an adopted session does not")
check(SessionState.none.survivingSessionEnd == .none, "nor does nothing at all")
// Which is the whole point: the survivor keeps its own screen across the restart.
check(compose(.dongleUp, SessionState.foreign(device: "esp32-car").survivingSessionEnd, nil, nil)
        == .wrongCar(device: "esp32-car"), "the wrong-car screen holds across a session end")

// The staleness threshold is a frame count against the contract's telemetry rate, not a duration
// picked here — and it is deliberately generous. A gap that costs one screen swap and back is
// worse than a late notice: the car's own 300 ms control watchdog is what protects the driving.
check(LinkRule.staleAfter == 12 / Double(CarContract.telemetryHz), "staleness from the contract")
// The gap that used to tear the drive screen down. Twelve frames is what makes it survivable.
check(compose(.dongleUp, adopted, fresh, 1.0).isLive,
      "a one-second gap no longer replaces the drive screen — UDP over a relay does that")
check(compose(.dongleUp, adopted, fresh, LinkRule.staleAfter - 0.01).isLive, "just inside the window")
check(!compose(.dongleUp, adopted, fresh, LinkRule.staleAfter).isLive, "the boundary is not live")

if failures == 0 { print("test_carlink: OK") } else { exit(1) }
