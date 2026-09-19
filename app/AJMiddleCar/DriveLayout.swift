import CoreGraphics

/// Where the drive screen's pieces go. Pure: the screen's size and safe-area insets in, points
/// out, so the design's numbers can be checked without a simulator (`app/tests/drivelayout`).
///
/// The picture is a 16:9 window the screen's full height, centred on the *screen*, not the safe
/// frame: the bands either side of it are what the safe insets mostly cover. The frame on the
/// wire is already 16:9 — the car crops the fisheye's 4:3 sensor frame to its middle rows at
/// the encoder's input (`frame_crop.h`), the top and bottom eighths being the fisheye's worst —
/// so the window trims nothing on a 16:9 screen and only the sides on a squatter one.
/// Everything else hangs off the picture's edges rather than the screen's, so a wider or
/// narrower phone moves the instruments with the picture. Points are in the safe-area frame
/// the ZStack lays out in, which is why a stick astride the picture's left edge sits left of
/// zero.
struct DriveLayout {
    struct Insets {
        let top: CGFloat, leading: CGFloat, bottom: CGFloat, trailing: CGFloat
    }

    /// The window's shape — the wire's own, `video.width` : `video.height`.
    static let pictureAspect: CGFloat = 16.0 / 9.0

    let screen: CGSize
    let insets: Insets

    private var safeWidth: CGFloat { screen.width - insets.leading - insets.trailing }
    private var safeHeight: CGFloat { screen.height - insets.top - insets.bottom }

    /// The picture's frame, in safe-frame coordinates. Never wider than the screen.
    var picture: CGRect {
        let w = min(screen.width, screen.height * Self.pictureAspect)
        return CGRect(x: (screen.width - w) / 2 - insets.leading, y: -insets.top, width: w, height: screen.height)
    }

    /// Padding from the safe frame's leading (and trailing — the picture is centred) edge to the
    /// top row, which hangs 14 pt inside the picture.
    var edge: CGFloat { picture.minX + 14 }

    /// Sticks astride the picture's edges, 16 pt above the safe bottom as before.
    var leftStick: CGPoint { CGPoint(x: picture.minX, y: safeHeight - 16 - 61) }
    var rightStick: CGPoint { CGPoint(x: picture.maxX, y: safeHeight - 16 - 61) }

    /// The PowerBar · DriveDiagram · PowerBar group, centred 50 pt above the safe bottom — the
    /// car body sits on the bottom edge, the rails run up over the picture's floor.
    var diagram: CGPoint { CGPoint(x: safeWidth / 2, y: safeHeight - 50) }

    /// The tricks button, in the middle of the band between the picture and the screen's edge —
    /// the screen's, not the safe frame's: nothing on the right side needs avoiding.
    var tricks: CGPoint { CGPoint(x: (picture.maxX + safeWidth + insets.trailing) / 2, y: safeHeight / 2) }

    /// Where the no-picture panel goes: the middle of the picture.
    var pictureCentre: CGPoint { CGPoint(x: safeWidth / 2, y: safeHeight / 2) }
}
