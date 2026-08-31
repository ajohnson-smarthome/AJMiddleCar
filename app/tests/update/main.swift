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

if failures == 0 { print("test_update: OK") } else { exit(1) }
