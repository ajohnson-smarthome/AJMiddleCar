// Host test for GateRule — whether a launch may hand the car over to the driver.
//
// The rule is absolute by decision: on the newest firmware, and *known* to be. What is pinned
// here is mostly the second half, because that is the half that used to leak — a launch that
// could not reach GitHub compared against nothing, concluded "nothing to update", and drove.
import Foundation

var failures = 0
func check(_ ok: Bool, _ what: String) {
    if !ok { print("FAIL: \(what)"); failures += 1 }
}

// The ordinary answers.
check(GateRule.mayDrive(deviceBuild: 717, latestBuild: 717), "current firmware drives")
check(GateRule.mayDrive(deviceBuild: 718, latestBuild: 717),
      "a device ahead of the newest release drives — a bench build is not a reason to refuse")
check(!GateRule.mayDrive(deviceBuild: 716, latestBuild: 717), "one build behind does not drive")
check(!GateRule.mayDrive(deviceBuild: 0, latestBuild: 717), "far behind does not drive")

// The leak this rule closes. "I could not check" must never read as "it is current".
check(!GateRule.mayDrive(deviceBuild: 717, latestBuild: nil),
      "an unknown newest release refuses, however current the device looks")
check(!GateRule.mayDrive(deviceBuild: nil, latestBuild: 717),
      "a device whose own version cannot be read refuses")
check(!GateRule.mayDrive(deviceBuild: nil, latestBuild: nil), "two unknowns are not a pass")

// The two failures are told apart, because they need different instructions.
check(!GateRule.canVerify(latestBuild: nil), "no release established means nothing was verified")
check(GateRule.canVerify(latestBuild: 717), "a release with a build number is verifiable")
check(GateRule.canVerify(latestBuild: 0), "build zero is a number, not an absence")

// The property that matters more than any single case: nothing unknown ever drives.
for device in [nil, 0, 716, 717, 9999] as [Int?] {
    check(!GateRule.mayDrive(deviceBuild: device, latestBuild: nil),
          "no device build drives without a known release (device: \(String(describing: device)))")
}

if failures == 0 { print("gate: all checks passed") }
exit(failures == 0 ? 0 : 1)
