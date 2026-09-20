// Host test for DriveLayout — where the drive screen's pieces go, in numbers.
//
// The numbers are the design's (docs/superpowers/specs/2026-09-15-drive-hud-design.md): an
// iPhone 17 in landscape is 874×402 pt with 59-pt side insets and a 21-pt bottom one, the
// picture is the 4:3 frame cropped to 16:9 and centred, and everything else hangs off the
// picture's edges. Points are in the safe-area frame the ZStack lays out in, so a stick
// astride the picture's edge lands left of zero — that is intended, not a bug.
import Foundation

var failures = 0
func check(_ ok: Bool, _ what: String) {
    if !ok { print("FAIL: \(what)"); failures += 1 }
}
func near(_ a: CGFloat, _ b: CGFloat) -> Bool { abs(a - b) < 0.01 }
func near(_ a: CGPoint, _ b: CGPoint) -> Bool { near(a.x, b.x) && near(a.y, b.y) }

let phone = DriveLayout(screen: CGSize(width: 874, height: 402),
                        insets: DriveLayout.Insets(top: 0, leading: 59, bottom: 21, trailing: 59))

// The picture: as tall as the screen, 16:9 of that, centred — 714.67 wide, so 79.67 in from
// the screen's edge and 20.67 in from the safe frame's.
check(near(phone.picture.width, 714.667), "picture is 16:9 of the screen's height")
check(near(phone.picture.height, 402), "picture is the screen's full height")
check(near(phone.picture.minX, 20.667), "picture is centred on the screen, not the safe frame")
check(near(phone.picture.minY, 0), "picture starts at the top")

// The top row sits 14 pt in from the picture's edges, so from the safe frame that is 34.67.
check(near(phone.edge, 34.667), "top row hangs 14 pt inside the picture's edge")

// Sticks astride the picture's edges, 16 pt above the safe bottom as today.
check(near(phone.leftStick, CGPoint(x: 20.667, y: 304)), "left stick centred on the picture's left edge")
check(near(phone.rightStick, CGPoint(x: 735.333, y: 304)), "right stick centred on the picture's right edge")

// The diagram group on the bottom edge, 50 pt above the safe bottom; the no-picture panel in
// the middle of the picture. The tricks button has no point of its own: it is a segment of
// the control bar in the top row, whose place `edge` already sets.
check(near(phone.diagram, CGPoint(x: 378, y: 331)), "diagram group centred 50 pt above the safe bottom")
check(near(phone.pictureCentre, CGPoint(x: 378, y: 190.5)), "no-picture panel in the middle of the picture")

// A narrower phone (iPhone 16, 852×393) gets a proportionally narrower picture — nothing here
// is a constant tuned to one model.
let narrower = DriveLayout(screen: CGSize(width: 852, height: 393),
                           insets: DriveLayout.Insets(top: 0, leading: 59, bottom: 21, trailing: 59))
check(near(narrower.picture.width, 698.667), "picture width follows the screen's height")
check(near(narrower.leftStick.x, (852 - 698.667) / 2 - 59), "stick follows the picture's edge")

// A screen too narrow for 16:9 of its height shows the picture edge to edge instead.
let squat = DriveLayout(screen: CGSize(width: 600, height: 402),
                        insets: DriveLayout.Insets(top: 0, leading: 0, bottom: 0, trailing: 0))
check(near(squat.picture.width, 600), "picture never exceeds the screen's width")
check(near(squat.picture.minX, 0), "a full-width picture starts at the edge")

if failures == 0 { print("drivelayout: ok") } else { print("drivelayout: \(failures) failure(s)"); exit(1) }
