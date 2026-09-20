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
                      video: VideoInfo(state: .idle, fps: 0, kbps: 0, dropped: 0),
                      battery: BatteryInfo(voltage_mv: 12310, current_ma: 3100, power_mw: 38200,
                                           soc_pct: 72, state: .ok))

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
// Identity is no longer judged in the live session — the gate's /version check (S23) is the one
// identity gate. A hello from any car adopts the session; a foreign car is driven, not screened
// (a mid-session swap on a colliding SSID is the accepted residual — spec wrong-car-runtime-out).
let foreignAdopted = SessionState.adopted(device: "esp32-car", fw: "v1.0+1")
check(compose(.dongleUp, foreignAdopted, fresh, 0.1) == .live(fresh),
      "any hello adopts: a foreign car is live, not a wrong-car screen")

// The staleness threshold is a frame count against the contract's telemetry rate, not a duration
// picked here — and it is deliberately generous. A gap that costs one screen swap and back is
// worse than a late notice: the car's own 300 ms control watchdog is what protects the driving.
check(LinkRule.staleAfter == 12 / Double(CarContract.telemetryHz), "staleness from the contract")
// The gap that used to tear the drive screen down. Twelve frames is what makes it survivable.
check(compose(.dongleUp, adopted, fresh, 1.0).isLive,
      "a one-second gap no longer replaces the drive screen — UDP over a relay does that")
check(compose(.dongleUp, adopted, fresh, LinkRule.staleAfter - 0.01).isLive, "just inside the window")
check(!compose(.dongleUp, adopted, fresh, LinkRule.staleAfter).isLive, "the boundary is not live")

// Whether the drive screen, once shown, still stands (AJM-107). `.searching` folds "the session
// is open but telemetry is late" and "there is no session" into one word, which is right for
// the label and wrong for the screen's existence: a telemetry pause shorter than the stall that
// ends the session used to swap the drive screen for the radar and take the sheets on it —
// the wizard's assignments, the settings stack — along. The screen is born on `.live` and lives
// while this holds: path up, session adopted, and a frame seen in this session.
func held(_ p: PathState, _ s: SessionState, _ t: Telemetry?) -> Bool {
    LinkRule.inSession(path: p, session: s, telemetry: t)
}
check(held(.dongleUp, adopted, fresh), "adopted with a frame: the drive screen stands")
check(!held(.dongleUp, adopted, nil), "adopted, no frame yet: not born — the first frame opens the screen")
check(!held(.dongleUp, .none, fresh), "no session: whatever frame is remembered, the screen goes")
check(!held(.noDongle(.notAvailable), adopted, fresh), "path gone: the screen goes with it")
check(!held(.localNetworkDenied, adopted, fresh), "denial: same")
// The point: stale telemetry inside a live session is `.searching` for the label and a held
// screen for the root — the two are asked separately, and disagree exactly here.
check(compose(.dongleUp, adopted, fresh, 2.6) == .searching && held(.dongleUp, adopted, fresh),
      "2.6 s of silence: «Поиск…» over the drive screen, not the radar instead of it")

if failures == 0 { print("test_carlink: OK") } else { exit(1) }
