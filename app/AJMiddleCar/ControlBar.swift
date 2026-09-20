import SwiftUI

/// The drive screen's control bar: one capsule of three segments in the top row, right of the
/// scheme toggle — tricks · video · settings, left to right. The bar is only the frame and the
/// dividers; the segments are the parent's views, each a button the size of its slot
/// (`ControlBarSegment.size`), so the bar decides where the buttons stand and nothing about
/// what they do. Built from the same tokens as the scheme toggle next to it: `segOff` for the
/// body, `line` for the stroke and the dividers — and the same metrics (`HudPill`), so the two
/// pills in the row share their height and their corners.
///
/// No clip on the capsule: the tricks segment hangs its card below the bar as an overlay, and
/// a `clipShape` here would cut it off at the bar's edge.
struct ControlBar<Tricks: View, Video: View, Settings: View>: View {
    let palette: Palette
    @ViewBuilder let tricks: () -> Tricks
    @ViewBuilder let video: () -> Video
    @ViewBuilder let settings: () -> Settings

    var body: some View {
        HStack(spacing: 0) {
            slot { tricks() }
            divider
            slot { video() }
            divider
            slot { settings() }
        }
        .background(RoundedRectangle(cornerRadius: HudPill.radius).fill(palette.segOff))
        .overlay(RoundedRectangle(cornerRadius: HudPill.radius).stroke(palette.line))
    }

    private func slot<V: View>(@ViewBuilder _ content: () -> V) -> some View {
        content().frame(width: ControlBarSegment.size.width, height: ControlBarSegment.size.height)
    }

    private var divider: some View {
        Rectangle().fill(palette.line).frame(width: 1, height: ControlBarSegment.size.height)
    }
}

/// The two pills of the top row — the scheme toggle and this bar — share one height and one
/// corner radius, so they stand on the same lines and neither looks like the other's
/// afterthought (`app/drive-hud`, «Две пилюли»). The bodies are each pill's own: the toggle is
/// clear with a `line` stroke, the bar sits on `segOff`.
enum HudPill {
    static let height: CGFloat = 32
    static let radius: CGFloat = 10
}

/// One segment's slot, 44 × `HudPill.height` — what each of the three buttons sizes itself to.
/// Outside the bar's type because that is generic over its segments, and the segments only want
/// the number.
enum ControlBarSegment {
    static let size = CGSize(width: 44, height: HudPill.height)
}
