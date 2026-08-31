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

// -- one release, two images: asset name and cache path per device --------------------
check(UpdateRules.Device.car.assetName == "\(CarContract.device).bin",
      "car asset name derives from the contract, not a second literal")
check(UpdateRules.Device.dongle.assetName == "\(DongleContract.device).bin",
      "dongle asset name derives from the contract, not a second literal")
check(UpdateRules.Device.car.assetName == "ajmiddlecar.bin", "car asset name matches the release")
check(UpdateRules.Device.dongle.assetName == "ajdongle.bin", "dongle asset name matches the release")
check(UpdateRules.Device.car.assetName != UpdateRules.Device.dongle.assetName,
      "the two images can never share an asset name")

check(UpdateRules.Device.car.cacheFileName == UpdateRules.Device.car.assetName,
      "cache filename is derived from the asset name, not spelled a second time")
check(UpdateRules.Device.dongle.cacheFileName == UpdateRules.Device.dongle.assetName,
      "cache filename is derived from the asset name, not spelled a second time")
check(UpdateRules.Device.car.cacheFileName != UpdateRules.Device.dongle.cacheFileName,
      "a cached image for one device is never offered for the other")

// -- cacheURL: the pure join that turns a device into an actual file path -------------
// This is the layer the risk actually lives at — `UpdateClient.cachedBinURL(for:)` is a thin,
// untested wrapper around it (it needs a real Application Support directory, which is why it
// stays in UpdateClient); this proves the join itself, with a directory the test controls.
let supportDir = URL(fileURLWithPath: "/tmp/update-rules-test-support")
check(UpdateRules.cacheURL(for: .car, in: supportDir) == UpdateRules.cacheURL(for: .car, in: supportDir),
      "same device, same directory → the same URL every time")
check(UpdateRules.cacheURL(for: .dongle, in: supportDir) == UpdateRules.cacheURL(for: .dongle, in: supportDir),
      "same device, same directory → the same URL every time (dongle side too)")
check(UpdateRules.cacheURL(for: .car, in: supportDir) != UpdateRules.cacheURL(for: .dongle, in: supportDir),
      "a URL built for one device is never the URL built for the other — the property that matters")
check(UpdateRules.cacheURL(for: .car, in: supportDir).lastPathComponent == UpdateRules.Device.car.assetName,
      "the join actually uses the device's asset name, not some other distinct-but-wrong name")
check(UpdateRules.cacheURL(for: .dongle, in: supportDir).lastPathComponent == UpdateRules.Device.dongle.assetName,
      "the join actually uses the device's asset name, not some other distinct-but-wrong name")

// -- mustUpdate answerable for either device, independently ---------------------------
// Same tag, same function, fed each device's own running firmware: the car being behind
// must not force the dongle, and vice versa — each answer depends only on its own input.
let latest = "v1.0+584"
check(UpdateRules.mustUpdate(carFw: "v1.0+500", latestTag: latest),
      "car behind the shared release → forced")
check(!UpdateRules.mustUpdate(carFw: "v1.0+584", latestTag: latest),
      "dongle current on the same shared release → free, unaffected by the car's gate")
check(UpdateRules.mustUpdate(carFw: "v1.0+400", latestTag: latest),
      "dongle behind the shared release → forced, computed the same way as the car's")
check(!UpdateRules.mustUpdate(carFw: "v1.0+700", latestTag: latest),
      "car ahead (dev build) → free, does not mask a dongle that is behind")

// -- flashPlan: the pure decision behind AppFlow.performDongleUpdate's device confusion risk --
// Everything below is the one guarantee Task 4's own review flagged as unproven until a call
// site existed to test: given a release and a cache state for a SPECIFIC device, flashPlan must
// carry THAT device through to whatever it returns, so a caller that names it once at the top
// (`flashPlan(for: .dongle, ...)`) has nothing left to independently mis-type downstream.
let dongleURL = URL(string: "https://example.invalid/ajdongle.bin")!
let carURL = URL(string: "https://example.invalid/ajmiddlecar.bin")!

// A fresh release, nothing cached yet: must download, and must carry .dongle through untouched.
switch UpdateRules.flashPlan(for: .dongle, release: (tag: "v1.0+200", assetURL: dongleURL),
                             cachedBuild: nil, hasCachedFile: false) {
case .download(let device, let url, let recordBuild, let tag):
    check(device == .dongle, "a plan built for .dongle carries .dongle, not .car")
    check(url == dongleURL, "the download URL is the one passed in for this device")
    check(recordBuild == 200, "the build to record is parsed from the release tag")
    check(tag == "v1.0+200", "the tag to record is the release's own tag")
default:
    check(false, "no cache and a newer release → .download")
}

// The identical inputs, requested for .car instead: must carry .car, not silently reuse .dongle
// from a shared default or a copy-paste of the case above.
switch UpdateRules.flashPlan(for: .car, release: (tag: "v1.0+200", assetURL: carURL),
                             cachedBuild: nil, hasCachedFile: false) {
case .download(let device, let url, _, _):
    check(device == .car, "a plan built for .car carries .car, not .dongle")
    check(url == carURL, "the download URL is the one passed in for .car, not .dongle's")
default:
    check(false, "no cache and a newer release → .download")
}

// Already current, already cached: reuse the cache, still carrying the right device.
switch UpdateRules.flashPlan(for: .dongle, release: (tag: "v1.0+200", assetURL: dongleURL),
                             cachedBuild: 200, hasCachedFile: true) {
case .useCache(let device):
    check(device == .dongle, "a cache-reuse plan for .dongle carries .dongle")
default:
    check(false, "current build, already cached → .useCache")
}

// No fresh release (GitHub unreachable) but a cache that is already current: reuse it — this is
// the offline path dongleGate()'s GateRule fallback feeds into flashPlan.
switch UpdateRules.flashPlan(for: .dongle, release: nil, cachedBuild: 150, hasCachedFile: true) {
case .useCache(let device):
    check(device == .dongle, "an offline cache-reuse plan still carries the right device")
default:
    check(false, "no release info at all, but a cached file exists and nothing says it is stale → .useCache")
}

// A newer release exists but there is nothing to fetch it from (offline) and nothing cached
// either: genuinely nothing to flash. This is the case that used to loop forever re-showing
// "updating" without ever making progress.
check(UpdateRules.flashPlan(for: .dongle, release: nil, cachedBuild: nil, hasCachedFile: false) == .unavailable,
      "nothing fresh, nothing cached → .unavailable, not an infinite retry")

// No fresh release AND no cached file, even with a recorded build number left over from some
// earlier session (e.g. the file was since cleared): a bare number is not something to flash.
// GateRule's own offline-fallback precondition (`hasCachedFile`) should make this unreachable
// on the path dongleGate() actually takes, but flashPlan does not trust that from here either.
check(UpdateRules.flashPlan(for: .dongle, release: nil, cachedBuild: 100, hasCachedFile: false) == .unavailable,
      "a recorded build with no backing file is not something to flash")

if failures == 0 { print("test_update: OK") } else { exit(1) }
