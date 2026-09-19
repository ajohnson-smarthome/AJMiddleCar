// Host test for StageRule.decide — one poll of one board's stage: what to show, whether to
// fetch the release first, advance, or fall back. Pure over reach + /version reply + tag.
// Run with swiftc; no XCTest, no simulator.
import Foundation
import Network

var failures = 0
func check(_ ok: Bool, _ what: String) { if !ok { print("FAIL: \(what)"); failures += 1 } }

let latest = "v1.0+200"
let dongle = Board.Identity(device: .dongle, expectedDevice: "ajdongle", silentStep: .absent)
let car    = Board.Identity(device: .car,    expectedDevice: "ajmiddlecar", silentStep: .seeking)

func doc(_ device: String, fw: String, proto: Int, rolledBack: Bool = false) -> VersionReply {
    .version(DeviceVersion(device: device, fw: fw, build: UpdateRules.buildNumber(fw) ?? -1,
                           proto: proto, rolled_back: rolledBack))
}
func decide(_ reach: Reach, _ version: VersionReply?, _ b: Board.Identity,
            tag: String? = latest, flashed: String? = nil,
            rollback: RollbackChoice = .unanswered) -> StageRule.Verdict {
    StageRule.decide(reach: reach, version: version, board: b, latestTag: tag, flashed: flashed,
                     rollback: rollback)
}

// reach speaks first, before /version is even read.
check(decide(.lost, nil, car) == .lost, "reach lost: fall back")
check(decide(.hold(.searching), nil, car) == .show(.searching), "reach hold: show that step")
check(decide(.hold(.joinFailed), nil, car) == .show(.joinFailed), "reach hold join-failed")

// A board that answered (a document or 404) with no tag yet needs the release first.
check(decide(.reached, .absent, dongle, tag: nil) == .needRelease, "404 + no tag: fetch release")
check(decide(.reached, doc("ajdongle", fw: "v1.0+100", proto: 1), dongle, tag: nil) == .needRelease,
      "document + no tag: fetch release")
// The second board: the tag learned on the adapter's stage serves the car — no second fetch, the
// rule is applied. The release was adopted only with both images (`UpdateRules.images(in:)`), so
// the car's image is in it by construction; nothing here re-checks that per board.
check(decide(.reached, doc("ajmiddlecar", fw: "v1.0+100", proto: 2), car, tag: latest) != .needRelease,
      "document + known tag: the rule runs, the release is not asked for again")
// Silence is not "answered": no release fetch, straight to the board's silent step.
check(decide(.reached, .silent, dongle, tag: nil) == .show(.absent), "silence + no tag: adapter absent, no fetch")
check(decide(.reached, .silent, car, tag: nil) == .show(.seeking), "silence + no tag: car seeking, no fetch")

// The rule's verdicts map to steps, per board's silent step.
check(decide(.reached, .silent, dongle) == .show(.absent), "silence: adapter .absent")
check(decide(.reached, .silent, car) == .show(.seeking), "silence: car .seeking")
check(decide(.reached, .faulty, car) == .show(.fault), "faulty: .fault")
check(decide(.reached, .denied, dongle) == .show(.denied), "denied: .denied")
check(decide(.reached, doc("esp32-car", fw: "v1.0+200", proto: 2), car) == .show(.wrongDevice("esp32-car")),
      "foreign device: .wrongDevice with its name")
check(decide(.reached, doc("ajmiddlecar", fw: "v1.0+200", proto: 2, rolledBack: true), car) == .show(.rolledBack),
      "rolled back: .rolledBack")
check(decide(.reached, doc("ajmiddlecar", fw: "v1.0+100", proto: 2, rolledBack: true), car, flashed: "v1.0+150")
      == .show(.updating),
      "rolled back from a build older than the tag: .updating — the phone's record reaches the rule (AJM-132)")
check(decide(.reached, doc("ajmiddlecar", fw: "v1.0+100", proto: 2), car) == .show(.updating),
      "behind the tag: .updating")
check(decide(.reached, .absent, car) == .show(.updating), "404: .updating (board older than /version)")
check(decide(.reached, doc("ajmiddlecar", fw: "v1.0+200", proto: 3), car) == .ok,
      "current build, any proto: .ok — proto is no longer a gate input")
check(decide(.reached, doc("ajmiddlecar", fw: "v1.0+200", proto: 2), car) == .ok, "current + our proto: .ok")

if failures == 0 { print("stagerule: all checks passed") } else { print("stagerule: \(failures) FAILED"); exit(1) }
