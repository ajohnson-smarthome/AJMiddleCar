// Host test for DriveModeRule — which drive screen to show and whether to subscribe to
// video, from the car's `video` config and whether a sheet covers the screen
// (docs/superpowers/specs/2026-09-15-video-switch-design.md, §3).
import Foundation

var failures = 0
func check(_ ok: Bool, _ what: String) {
    if !ok { print("FAIL: \(what)"); failures += 1 }
}

// The config not read yet: the layout from before video, and no subscription — the car
// would ignore the views if the switch is off, and the HUD would flash if it is on.
check(DriveModeRule.state(config: nil, covered: false) == DriveScreenState(mode: .classic, watching: false),
      "no config yet: classic, not watching")

// Off: classic, not watching, sheet or no sheet.
check(DriveModeRule.state(config: Video(bitrate_kbps: 2500, enabled: false), covered: false)
      == DriveScreenState(mode: .classic, watching: false), "off: classic, not watching")
check(DriveModeRule.state(config: Video(bitrate_kbps: 2500, enabled: false), covered: true)
      == DriveScreenState(mode: .classic, watching: false), "off under a sheet: still classic")

// On: the HUD; a sheet over it stops the views but not the layout underneath.
check(DriveModeRule.state(config: Video(bitrate_kbps: 2500, enabled: true), covered: false)
      == DriveScreenState(mode: .hud, watching: true), "on: hud, watching")
check(DriveModeRule.state(config: Video(bitrate_kbps: 2500, enabled: true), covered: true)
      == DriveScreenState(mode: .hud, watching: false), "on under a sheet: hud, not watching")

if failures > 0 { print("drivemode: \(failures) failure(s)"); exit(1) }
print("drivemode: ok")
