import SwiftUI

/// One instrument of the top row's left cluster — a glyph and a caption, drawn by one rule for
/// all three (`app/drive-hud`, «Приборы показывают связь и намерение пульта»): the glyph in a
/// box as tall as the link's bars, one gap to the caption, one caption size, tabular digits so
/// a number that changes does not push its neighbours, and one colour for the whole
/// instrument. `tint` is set as the parent's `foregroundStyle`, so a glyph that draws itself in
/// the instrument's colour (an `Image`) inherits it, while one that carries its own state
/// colours (`SignalBars`, the battery icon) keeps them — the caption is then what wears the
/// instrument's colour.
struct HudItem<Glyph: View>: View {
    let glyph: Glyph
    let caption: String
    let tint: Color

    var body: some View {
        HStack(spacing: HudCluster.gap) {
            glyph.frame(height: HudCluster.glyphHeight)
            Text(caption)
                .font(.system(size: HudCluster.captionSize))
                .monospacedDigit()
        }
        .foregroundStyle(tint)
    }
}

/// The line between two instruments of the cluster: 1 pt of `line`, as tall as a glyph's box —
/// the rhythm the bar's dividers give the right side of the row, so the left has one too.
struct HudDivider: View {
    let palette: Palette

    var body: some View {
        Rectangle().fill(palette.line).frame(width: 1, height: HudCluster.glyphHeight)
    }
}

/// The left cluster's numbers, beside `HudPill`'s for the right — the canvas's (`design.md`,
/// decision 3): 14 pt is the height of the link's bars, which the other glyphs are pulled to;
/// 11 pt sits between the old 12 of the link and the 10 of the battery, so the cluster does not
/// outweigh the pills; 6 is the gap inside an instrument, 14 the gap between them.
enum HudCluster {
    static let glyphHeight: CGFloat = 14
    static let captionSize: CGFloat = 11
    static let gap: CGFloat = 6
    static let spacing: CGFloat = 14
}
