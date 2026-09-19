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
func step(_ reply: VersionReply, latest tag: String = latest, flashed: String? = nil,
          rollback: RollbackChoice = .unanswered) -> VersionStep {
    VersionRule.step(reply: reply, expectedDevice: "ajdongle", latestTag: tag, flashed: flashed,
                     rollback: rollback)
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

// -- the reference point is the build that rolled back, not the tag on hand (AJM-132) -----------
// The phone records the last build it flashed into each board at `ok`. In any launch after the
// rollback the ladder learns the NEWEST tag before the rollback screen, so measuring "newer"
// from the tag on hand at «Повторить» measured it from itself: release 890 rolled back, the
// fixing 900 shipped, and «Повторить» answered «откат» forever. With the record, 900 is newer
// than what rolled back (890) and newer than what the board runs (880): update — and no tap
// is needed for that, the record answers the first look.
check(step(doc(fw: "v1.0+880", rolledBack: true), latest: "v1.0+900", flashed: "v1.0+890") == .updating,
      "rolled back from 890, release 900: update — without a tap, the record is the reference")
check(step(doc(fw: "v1.0+880", rolledBack: true), latest: "v1.0+890", flashed: "v1.0+890") == .rolledBack,
      "rolled back from 890, release still 890: the same image is not re-flashed into the same rollback")
check(step(doc(fw: "v1.0+880", rolledBack: true), latest: "v1.0+900", flashed: "v1.0+890",
           rollback: .recheck(from: "v1.0+900")) == .updating,
      "«Повторить» with the newest tag already on hand: the record wins over the tag on offer")
check(step(doc(fw: "v1.0+880", rolledBack: true), latest: "v1.0+890", flashed: "v1.0+890",
           rollback: .recheck(from: "v1.0+880")) == .rolledBack,
      "«Повторить» from an older tag on hand: the record still says nothing newer shipped")
check(step(doc(fw: "v1.0+900", rolledBack: true), latest: "v1.0+900", flashed: "v1.0+890") == .rolledBack,
      "release newer than the record, but the board already runs it: nothing to flash")
check(step(doc(fw: "v1.0+880", rolledBack: true), latest: "v1.0+900", flashed: nil) == .rolledBack,
      "no record and nothing said: the rollback stands until «Повторить»")
check(step(doc(fw: "v1.0+880", rolledBack: true), latest: "v1.0+900", flashed: "v1.0+890",
           rollback: .recheck(from: nil)) == .updating,
      "a record with a recheck from nothing on hand: measured from the record")

// -- version ------------------------------------------------------------------------------------
check(step(doc(fw: behind)) == .updating, "behind the release: update")
check(step(doc(fw: "v1.0")) == .updating, "a build without a number is behind")
check(step(doc(fw: current)) == .ok, "current: ok")
check(step(doc(fw: ahead)) == .ok, "ahead of the release: ok — a dev build from a cable")

check(step(doc(fw: current, proto: 2)) == .ok, "current build, any proto: ok — proto is not a gate input anymore")

if failures == 0 { print("test_versionrule: OK") } else { exit(1) }
