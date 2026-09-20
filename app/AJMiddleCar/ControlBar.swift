import SwiftUI

/// The drive screen's control bar: one capsule of three segments in the top row, right of the
/// scheme toggle — tricks · video · settings, left to right. The bar is only the frame and the
/// dividers; the segments are the parent's views, each a button the size of its slot
/// (`ControlBarSegment.size`), so the bar decides where the buttons stand and nothing about
/// what they do. Built from the same tokens as the scheme toggle next to it: `segOff` for the
/// body, `line` for the stroke and the dividers, radius 10.
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
        .background(RoundedRectangle(cornerRadius: 10).fill(palette.segOff))
        .overlay(RoundedRectangle(cornerRadius: 10).stroke(palette.line))
    }

    private func slot<V: View>(@ViewBuilder _ content: () -> V) -> some View {
        content().frame(width: ControlBarSegment.size.width, height: ControlBarSegment.size.height)
    }

    private var divider: some View {
        Rectangle().fill(palette.line).frame(width: 1, height: ControlBarSegment.size.height)
    }
}

/// One segment's slot, 44 × 32 — what each of the three buttons sizes itself to. Outside the
/// bar's type because that is generic over its segments, and the segments only want the number.
enum ControlBarSegment {
    static let size = CGSize(width: 44, height: 32)
}
