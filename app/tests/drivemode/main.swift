// Host test for DriveModeRule — whether to subscribe to video and whether the driver's input
// is taken, from the car's `video` config and whether something covers the screen (openspec
// `app/drive-session`, «Режим экрана вождения и подписка решаются вместе»). The layout is
// not the rule's to pick: the drive screen has one, and the switch only decides whether its
// window is live (`app/drive-hud`).
import Foundation

var failures = 0
func check(_ ok: Bool, _ what: String) {
    if !ok { print("FAIL: \(what)"); failures += 1 }
}

let on = Video(bitrate_kbps: 2500, enabled: true)
let off = Video(bitrate_kbps: 2500, enabled: false)

// The config not read yet: no subscription — the car would ignore the views if the switch is
// off, and the window would light up and go dark again if it is on. Driving as usual.
check(DriveModeRule.state(config: nil, covered: false)
      == DriveScreenState(watching: false, inputAllowed: true),
      "no config yet: not watching, driving")

// Off: not watching, sheet or no sheet.
check(DriveModeRule.state(config: off, covered: false)
      == DriveScreenState(watching: false, inputAllowed: true), "off: not watching, driving")
check(DriveModeRule.state(config: off, covered: true)
      == DriveScreenState(watching: false, inputAllowed: false), "off under a sheet: not watching, not driving")

// On: watching; a sheet over the screen stops the views.
check(DriveModeRule.state(config: on, covered: false)
      == DriveScreenState(watching: true, inputAllowed: true), "on: watching, driving")
check(DriveModeRule.state(config: on, covered: true)
      == DriveScreenState(watching: false, inputAllowed: false), "on under a sheet: not watching, not driving")

// The bug (AJM-101): a covered screen kept taking the gamepad, so a deflected stick drove an
// uncalibrated car under the mandatory wizard. Covered is covered — whatever the config says,
// the intent is nobody's to move until the sheet is gone.
check(!DriveModeRule.state(config: nil, covered: true).inputAllowed, "covered, no config: input refused")
check(!DriveModeRule.state(config: off, covered: true).inputAllowed, "covered, video off: input refused")
check(!DriveModeRule.state(config: on, covered: true).inputAllowed, "covered, video on: input refused")
check(DriveModeRule.state(config: on, covered: false).inputAllowed, "uncovered: input taken again")

// The video button (AJM-120): with the domain never read it used to be dead, and the drive
// screen has no other path to a retry. A tap on an unread domain re-reads; a tap on a read one
// toggles the switch and keeps the car's bitrate.
check(DriveModeRule.videoTap(config: nil) == .reread, "domain unread: the tap re-reads")
check(DriveModeRule.videoTap(config: on) == .toggle(Video(bitrate_kbps: 2500, enabled: false)),
      "on: the tap posts the domain with the switch off and the bitrate as it was")
check(DriveModeRule.videoTap(config: Video(bitrate_kbps: 1500, enabled: false))
      == .toggle(Video(bitrate_kbps: 1500, enabled: true)),
      "off: the tap posts the domain with the switch on and the bitrate as it was")

if failures > 0 { print("drivemode: \(failures) failure(s)"); exit(1) }
print("drivemode: ok")
