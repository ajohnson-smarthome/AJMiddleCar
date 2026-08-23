// Host test for the launch gate's offline rule. Run with swiftc; no XCTest, no simulator.
import Foundation

var failures = 0
func check(_ ok: Bool, _ what: String) {
    if !ok { print("FAIL: \(what)"); failures += 1 }
}

// GitHub being unreachable must not strand a phone next to a healthy car — but only when
// there is actually something to flash-compare later: a cached file AND its recorded build.
check(GateRule.canProceedOffline(hasCachedFile: true, cachedBuild: 517),
      "cached file + build proceeds offline")
check(!GateRule.canProceedOffline(hasCachedFile: false, cachedBuild: 517),
      "no file: the first-ever launch still needs the internet")
check(!GateRule.canProceedOffline(hasCachedFile: true, cachedBuild: nil),
      "file without a recorded build is not a known version")
check(!GateRule.canProceedOffline(hasCachedFile: false, cachedBuild: nil),
      "nothing cached, nothing to proceed with")

// Decision 4a: an offline launch seeds the forced gate with the last-known release, so
// mustUpdate can still compare — a cached tag is only trustworthy when the cached image
// it describes actually exists.
check(GateRule.offlineLatestTag(cachedTag: "v1.0+584", hasCachedFile: true, cachedBuild: 584)
        == "v1.0+584", "offline launch seeds the last-known tag")
check(GateRule.offlineLatestTag(cachedTag: "v1.0+584", hasCachedFile: false, cachedBuild: 584)
        == nil, "no file: the tag describes nothing flashable")
check(GateRule.offlineLatestTag(cachedTag: "v1.0+584", hasCachedFile: true, cachedBuild: nil)
        == nil, "no recorded build: not a known version")
check(GateRule.offlineLatestTag(cachedTag: nil, hasCachedFile: true, cachedBuild: 584)
        == nil, "no tag recorded, nothing to seed")

if failures == 0 { print("test_gate: OK") } else { exit(1) }
