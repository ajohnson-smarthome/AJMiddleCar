// Host test for the update decisions. Run with swiftc; no XCTest, no simulator.
import Foundation

var failures = 0
func check(_ ok: Bool, _ what: String) {
    if !ok { print("FAIL: \(what)"); failures += 1 }
}

// -- version parsing, against the strings the firmware actually ships -----------------
check(UpdateRules.buildNumber("v1.0+584") == 584, "firmware-shaped tag parses")
check(UpdateRules.buildNumber("v1.0") == nil, "no build number is nil")
check(UpdateRules.buildNumber(nil) == nil, "nil is nil")
check(UpdateRules.normalize("v1.2-3-gabc") == "1.2", "normalize strips v and -suffix")

// -- gate decisions -------------------------------------------------------------------
check(UpdateRules.mustUpdate(carFw: "v1.0+500", latestTag: "v1.0+584"), "behind → forced")
check(!UpdateRules.mustUpdate(carFw: "v1.0+584", latestTag: "v1.0+584"), "equal → free")
check(!UpdateRules.mustUpdate(carFw: "v1.0+600", latestTag: "v1.0+584"), "dev build ahead → free")
check(UpdateRules.mustUpdate(carFw: "v0.9", latestTag: "v1.0+584"), "pre-versioning car → forced")
check(!UpdateRules.mustUpdate(carFw: "v1.0+500", latestTag: nil), "no known release → gate inert")

check(UpdateRules.isUpdateAvailable(running: "v1.0+500", latest: "v1.0+584"), "newer is available")
check(!UpdateRules.isUpdateAvailable(running: "v1.0+584", latest: "v1.0+584"), "same is not")
check(UpdateRules.needsDownload(latestBuild: 584, cachedBuild: 500, hasCachedFile: true),
      "stale cache re-downloads")
check(!UpdateRules.needsDownload(latestBuild: 584, cachedBuild: 584, hasCachedFile: true),
      "current cache does not")

// -- decision 6: what may enter the firmware cache ------------------------------------
check(UpdateRules.isValidImage(firstByte: 0xE9, size: 763_088), "a real image passes")
check(!UpdateRules.isValidImage(firstByte: 0x3C, size: 763_088),
      "an HTML error page ('<') is not firmware")
check(!UpdateRules.isValidImage(firstByte: 0xE9, size: 4095), "under the 4 KB floor fails")
check(UpdateRules.isValidImage(firstByte: 0xE9, size: 4096), "exactly the floor passes")
check(!UpdateRules.isValidImage(firstByte: nil, size: 0), "an empty body fails")

if failures == 0 { print("test_update: OK") } else { exit(1) }
