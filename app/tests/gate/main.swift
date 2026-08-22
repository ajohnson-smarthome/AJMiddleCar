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

if failures == 0 { print("test_gate: OK") } else { exit(1) }
