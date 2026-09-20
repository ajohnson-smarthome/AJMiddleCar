import SwiftUI

/// The picture's instrument in the top row, between the link and the pack, only while there
/// is a picture: the camera glyph and «N к/с» as a `HudItem`, and — only while frames are
/// being lost — a second pair inside the same instrument, «пунктирный кадр · M» in `warn`,
/// with no divider between the two. What it shows is `VideoGauge`'s verdict, not its own: at
/// zero losses the instrument is the one pair, as the battery is silent until `low`. The word
/// «потеряно» is VoiceOver's alone (`accessibilityLabel`); the row shows the glyph.
///
/// SF `video` at 14 pt regular: the same family as the bar's glyphs, and at this size its
/// stroke matches the battery's 1 pt outline (`design.md`, decision 4).
struct VideoBadge: View {
    let fps: Int
    let lost: Int
    let palette: Palette

    var body: some View {
        let g = VideoGauge.make(fps: fps, lost: lost, words: .app)
        HStack(spacing: 10) {
            HudItem(glyph: Image(systemName: "video").font(.system(size: 14, weight: .regular)),
                    caption: g.caption, tint: palette.text)
            if let lost = g.lost {
                HStack(spacing: 4) {
                    Image(systemName: "rectangle.dashed").font(.system(size: 12, weight: .regular))
                    Text("\(lost)")
                        .font(.system(size: HudCluster.captionSize))
                        .monospacedDigit()
                }
                .foregroundStyle(palette.warn)
            }
        }
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(g.accessibility)
    }
}

extension VideoGauge.Words {
    /// The app's words, from `Localizable.strings`; the host test supplies the spec's.
    static let app = VideoGauge.Words(stats: L.videoStats(fps:), lost: L.videoLost(_:))
}
