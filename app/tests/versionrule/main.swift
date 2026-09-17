// Host test for VersionRule.step — the one decision the launch gate makes for either board
// from its /version: foreign, rolled back, behind, or ok. Run with swiftc.
import Foundation
import Network

var failures = 0
func check(_ ok: Bool, _ what: String) { if !ok { print("FAIL: \(what)"); failures += 1 } }

let latest = "v1.0+200"          // what GitHub says is newest
let behind = "v1.0+100"
let current = "v1.0+200"
let ahead = "v1.0+250"           // a board flashed from a newer main by cable
func doc(_ device: String = "ajdongle", fw: String, proto: Int = 1, rolledBack: Bool = false) -> VersionReply {
    .version(DeviceVersion(device: device, fw: fw, build: UpdateRules.buildNumber(fw) ?? -1,
                           proto: proto, rolled_back: rolledBack))
}
func step(_ reply: VersionReply, rollback: RollbackChoice = .unanswered) -> VersionStep {
    VersionRule.step(reply: reply, expectedDevice: "ajdongle", latestTag: latest, rollback: rollback)
}

// -- the three ways a read fails map one to one -----------------------------------------------
check(step(.silent) == .plugIn, "silence: plug it in")
check(step(.faulty) == .faulty, "a bad answer: faulty")
check(step(.denied) == .accessDenied, "denied: access denied")

// -- identity first, before any other field is believed -----------------------------------------
check(step(doc("someones-adapter", fw: behind)) == .wrongDevice(name: "someones-adapter"),
      "a foreign board is named, not flashed — even when it is behind")
check(step(doc("someones-adapter", fw: current, rolledBack: true)) == .wrongDevice(name: "someones-adapter"),
      "a foreign board's rollback flag is not read")

// -- a board that predates /version is behind ----------------------------------------------------
check(step(.absent) == .updating, "404 is a board older than the endpoint: update it")

// -- rollback -----------------------------------------------------------------------------------
check(step(doc(fw: behind, rolledBack: true)) == .rolledBack, "rolled back and unanswered: report it")
check(step(doc(fw: behind, rolledBack: true), rollback: .recheck(from: "v1.0+150")) == .updating,
      "rolled back, rechecked, and the release is newer than what was on offer AND than the board: update")
check(step(doc(fw: behind, rolledBack: true), rollback: .recheck(from: latest)) == .rolledBack,
      "rolled back, rechecked, nothing newer than what was on offer: back to the report")
check(step(doc(fw: current, rolledBack: true), rollback: .recheck(from: "v1.0+150")) == .rolledBack,
      "rolled back, a newer offer, but the board already runs it: nothing to flash")

// -- version ------------------------------------------------------------------------------------
check(step(doc(fw: behind)) == .updating, "behind the release: update")
check(step(doc(fw: "v1.0")) == .updating, "a build without a number is behind")
check(step(doc(fw: current)) == .ok, "current: ok")
check(step(doc(fw: ahead)) == .ok, "ahead of the release: ok — a dev build from a cable")

check(step(doc(fw: current, proto: 2)) == .ok, "current build, any proto: ok — proto is not a gate input anymore")

if failures == 0 { print("test_versionrule: OK") } else { exit(1) }
